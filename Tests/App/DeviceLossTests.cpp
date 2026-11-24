#include "DeviceLossTests.h"
#include "PhantomCore.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace PhantomCore;

namespace
{
    // What one engine run told its sink, in the order it happened. "shutdown"
    // is added by the Client so the order against its cleanup can be checked.
    struct RunLog
    {
        std::vector<std::string> events;
        std::vector<std::string> records;
        std::vector<GraphicsDeviceLossReport> losses;
        std::vector<std::string> fatalErrors;
        PerformanceStatistics sample;
    };

    class RecordingSink final : public IEngineDiagnosticSink
    {
      public:
        explicit RecordingSink(RunLog &log) : log_(log)
        {
        }
        void OnEngineRecord(const std::string_view line) noexcept override
        {
            try
            {
                log_.records.emplace_back(line);
            }
            catch (...)
            {
            }
        }
        void OnGraphicsDeviceLost(const GraphicsDeviceLossReport &report) noexcept override
        {
            try
            {
                log_.losses.push_back(report);
                log_.events.emplace_back("lost");
            }
            catch (...)
            {
            }
        }
        FatalErrorPresentation OnFatalError(const std::string_view message) noexcept override
        {
            try
            {
                log_.fatalErrors.emplace_back(message);
                log_.events.emplace_back("fatal");
            }
            catch (...)
            {
            }
            return FatalErrorPresentation::Handled;
        }

      private:
        RunLog &log_;
    };

    constexpr std::uint64_t FramesBeforeRemoval = 5;

    // Draws one panel every frame. With removeDevice the engine loses the
    // device after five frames; without it the run ends at the first
    // one-second performance sample.
    class DiagnosedClient final : public IGameClient
    {
      public:
        DiagnosedClient(RunLog &log, RecordingSink &sink, const bool removeDevice, const bool extendedData)
            : log_(log), sink_(sink), removeDevice_(removeDevice), extendedData_(extendedData)
        {
        }
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = 64;
            config.windowHeight = 64;
            config.renderRateOverrideHz = 240.0;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            config.diagnostics.sink = &sink_;
            config.diagnostics.graphicsDeviceRemovedData = extendedData_;
            config.diagnostics.removeGraphicsDeviceAfterRenderedFrames = removeDevice_ ? FramesBeforeRemoval : 0;
            return config;
        }
        void Initialize(const EngineServices &) override
        {
            static_cast<void>(CreatePanel(canvas_.Root(), {-32, -32, 32, 32}));
        }
        bool Update(const UpdateContext &context) override
        {
            if (context.performance.hasMeasurement)
                log_.sample = context.performance;
            const bool sampled = !removeDevice_ && context.performance.hasMeasurement;
            return !sampled && context.totalSeconds < 10.0;
        }
        void Render(const RenderContext &context) override
        {
            context.visual2DRendering->SubmitScreen(canvas_, context);
        }
        void OnResize(std::uint32_t, std::uint32_t) override
        {
        }
        void Shutdown() noexcept override
        {
            try
            {
                log_.events.emplace_back("shutdown");
            }
            catch (...)
            {
            }
        }

      private:
        RunLog &log_;
        RecordingSink &sink_;
        bool removeDevice_;
        bool extendedData_;
        Visual2DCanvas canvas_{{64, 64}};
    };

    // Each engine run gets its own thread, like the other device tests.
    int RunDiagnosed(RunLog &log, const bool removeDevice, const bool extendedData)
    {
        RecordingSink sink(log);
        int result = EXIT_SUCCESS;
        std::jthread engine(
            [&] { result = Run(std::make_unique<DiagnosedClient>(log, sink, removeDevice, extendedData)); });
        engine.join();
        return result;
    }

    bool HasRecord(const RunLog &log, const std::string_view prefix)
    {
        for (const std::string &record : log.records)
        {
            if (record.starts_with(prefix))
                return true;
        }
        return false;
    }

    bool Fail(const char *message)
    {
        std::cerr << message << '\n';
        return false;
    }

    bool CheckStartRecords(const RunLog &log, const bool extendedData)
    {
        const bool described = HasRecord(log, "graphics adapter: ") && HasRecord(log, "swap chain: 64x64, ");
        if (!described || !HasRecord(log, "window: 64x64 Windowed, "))
            return Fail("A diagnosed run must record the adapter, the swap chain and the window at start.");
        if (!HasRecord(log, extendedData ? "device removed data: on" : "device removed data: off"))
            return Fail("The start record must say whether device removed data is collected.");
        return true;
    }

    bool CheckHealthyRun()
    {
        RunLog log;
        if (RunDiagnosed(log, false, false) != EXIT_SUCCESS)
            return Fail("A run with a diagnostic sink and a working device must succeed.");
        if (!CheckStartRecords(log, false))
            return false;
        if (!log.losses.empty() || !log.fatalErrors.empty())
            return Fail("A working device must report neither a loss nor a fatal error.");
        if (log.records.size() != 4)
            return Fail("A run in which nothing changes must record only its four start lines.");
        if (!log.sample.hasMeasurement || log.sample.longestLoopSeconds <= 0.0)
            return Fail("The performance sample must carry the longest main-loop pass of its interval.");
        if (log.sample.graphicsMemoryUsageBytes > 0 && log.sample.graphicsMemoryBudgetBytes == 0)
            return Fail("Graphics memory use must come with the budget it is measured against.");
        std::cout << "Healthy run: " << log.records.size() << " start records, longest pass "
                  << log.sample.longestLoopSeconds * 1000.0 << " ms, graphics memory "
                  << log.sample.graphicsMemoryUsageBytes / (1024 * 1024) << " of "
                  << log.sample.graphicsMemoryBudgetBytes / (1024 * 1024) << " MB.\n";
        for (const std::string &record : log.records)
            std::cout << "  " << record << '\n';
        return true;
    }

    bool CheckLossReport(const GraphicsDeviceLossReport &report, const bool extendedData)
    {
        const bool named = !report.operation.empty() && report.resultCode != 0;
        if (!named || report.removedReason == 0 || report.removedReasonName.empty())
            return Fail("The loss report must name the failed call, its result and the removal reason.");
        const bool sized = report.width == 64 && report.height == 64;
        if (!sized || report.renderIndex < FramesBeforeRemoval)
            return Fail("The loss report must carry the back buffer size and the last frame begun.");
        if (report.extendedDataEnabled != extendedData)
            return Fail("The loss report must say whether device removed data was collected.");
        if (extendedData == report.extendedData.empty())
            return Fail("Device removed data must have lines exactly when it was requested.");
        return true;
    }

    bool CheckLostRun(const bool extendedData)
    {
        RunLog log;
        if (RunDiagnosed(log, true, extendedData) != EXIT_FAILURE)
            return Fail("A run whose device was removed must end as a failure.");
        if (!CheckStartRecords(log, extendedData))
            return false;
        if (log.losses.size() != 1 || log.fatalErrors.size() != 1)
            return Fail("A removed device must be reported once as a loss and once as the fatal error.");
        // The loss is described while the renderer exists, the Client is then
        // shut down, and the fatal error comes last.
        const std::vector<std::string> expected{"lost", "shutdown", "fatal"};
        if (log.events != expected)
            return Fail("The order must be: loss report, Client shutdown, fatal error.");
        if (log.fatalErrors.front().find("failed with HRESULT 0x") == std::string::npos)
            return Fail("The fatal error must keep the usual failed-call text.");
        const GraphicsDeviceLossReport &report = log.losses.front();
        if (!CheckLossReport(report, extendedData))
            return false;
        std::cout << "Removed device (device removed data " << (extendedData ? "on" : "off") << "): "
                  << report.operation << " -> " << report.removedReasonName << ", frame " << report.renderIndex << ", "
                  << report.extendedData.size() << " extended lines.\n";
        for (const std::string &line : report.extendedData)
            std::cout << "  " << line << '\n';
        return true;
    }
} // namespace

int TestDeviceLoss()
{
    // Device removed data is a process-wide switch, so the runs without it come first.
    // A device removed on request has no command list history; the run with the
    // data on checks that the request reaches the report, not what a real hang leaves.
    if (!CheckHealthyRun() || !CheckLostRun(false) || !CheckLostRun(true))
        return EXIT_FAILURE;
    std::cout << "Engine diagnostics passed: start records, performance sample, device loss report and order.\n";
    return EXIT_SUCCESS;
}
