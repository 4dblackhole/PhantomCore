#include "GraphicsOptionsTests.h"
#include "PhantomCore.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace PhantomCore;

namespace
{
    struct OptionRun
    {
        std::vector<std::string> records;
        std::uint32_t renderedFrames{};
        std::uint32_t framesAfterResize{};
        std::string resizeError;
    };

    class RecordingSink final : public IEngineDiagnosticSink
    {
      public:
        explicit RecordingSink(OptionRun &run) : run_(run)
        {
        }
        void OnEngineRecord(const std::string_view line) noexcept override
        {
            try
            {
                run_.records.emplace_back(line);
            }
            catch (...)
            {
            }
        }
        void OnGraphicsDeviceLost(const GraphicsDeviceLossReport &) noexcept override
        {
        }
        FatalErrorPresentation OnFatalError(std::string_view) noexcept override
        {
            return FatalErrorPresentation::Handled;
        }
        void OnMainLoopStalled(const MainLoopStallReport &) noexcept override
        {
        }
        void OnMainLoopResumed(double) noexcept override
        {
        }

      private:
        OptionRun &run_;
    };

    constexpr std::uint32_t FramesBeforeResize = 20;
    constexpr std::uint32_t FramesAfterResize = 30;
    constexpr std::uint32_t StartSize = 64;
    // Windows may widen a small window to its minimum; any size other than the
    // start size shows the swap chain was resized.
    constexpr WindowResolution ResizedTo{160, 120};

    // Draws one panel every frame, changes the window size after a few frames
    // (so the swap chain is resized with its creation flags) and ends once
    // enough frames were presented at the new size.
    class OptionsClient final : public IGameClient
    {
      public:
        OptionsClient(OptionRun &run, RecordingSink &sink, GraphicsDeviceDiagnosticsConfig graphics)
            : run_(run), sink_(sink), graphics_(std::move(graphics))
        {
        }
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = StartSize;
            config.windowHeight = StartSize;
            config.renderRateOverrideHz = 240.0;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            config.diagnostics.sink = &sink_;
            config.diagnostics.graphics = graphics_;
            return config;
        }
        void Initialize(const EngineServices &services) override
        {
            window_ = services.windowControl;
            static_cast<void>(CreatePanel(canvas_.Root(), {-32, -32, 32, 32}));
        }
        bool Update(const UpdateContext &context) override
        {
            if (!resizeRequested_ && run_.renderedFrames >= FramesBeforeResize && window_ != nullptr)
            {
                resizeRequested_ = true;
                if (!window_->SetClientSize(ResizedTo, run_.resizeError) && run_.resizeError.empty())
                    run_.resizeError = "SetClientSize refused";
            }
            return run_.framesAfterResize < FramesAfterResize && context.totalSeconds < 20.0;
        }
        void Render(const RenderContext &context) override
        {
            context.visual2DRendering->SubmitScreen(canvas_, context);
            ++run_.renderedFrames;
            if (context.width != StartSize || context.height != StartSize)
                ++run_.framesAfterResize;
        }
        void OnResize(std::uint32_t, std::uint32_t) override
        {
        }
        void Shutdown() noexcept override
        {
        }

      private:
        OptionRun &run_;
        RecordingSink &sink_;
        GraphicsDeviceDiagnosticsConfig graphics_;
        IWindowControl *window_{};
        bool resizeRequested_{};
        Visual2DCanvas canvas_{{64, 64}};
    };

    int RunWith(OptionRun &run, const GraphicsDeviceDiagnosticsConfig &graphics)
    {
        RecordingSink sink(run);
        int result = EXIT_SUCCESS;
        std::jthread engine([&] { result = Run(std::make_unique<OptionsClient>(run, sink, graphics)); });
        engine.join();
        return result;
    }

    bool Fail(const char *message)
    {
        std::cerr << message << '\n';
        return false;
    }

    // The first record that starts with prefix, or empty.
    std::string RecordStartingWith(const OptionRun &run, const std::string_view prefix)
    {
        for (const std::string &record : run.records)
        {
            if (record.starts_with(prefix))
                return record;
        }
        return {};
    }

    bool Contains(const std::string &text, const std::string_view part)
    {
        return text.find(part) != std::string::npos;
    }

    void Print(const char *title, const OptionRun &run)
    {
        std::cout << title << ": " << run.renderedFrames << " frames, " << run.framesAfterResize << " after resize\n";
        for (const std::string &record : run.records)
            std::cout << "  " << record << '\n';
    }

    // Runs one configuration and checks that it rendered through a resize.
    bool RunRendered(const char *title, OptionRun &run, const GraphicsDeviceDiagnosticsConfig &graphics)
    {
        if (RunWith(run, graphics) != EXIT_SUCCESS)
            return Fail("A run with these graphics options must succeed.");
        Print(title, run);
        if (!run.resizeError.empty())
            return Fail("The window must accept the test's size change.");
        if (run.framesAfterResize < FramesAfterResize)
            return Fail("The run must keep presenting after the swap chain was resized.");
        return true;
    }

    bool CheckDefaults()
    {
        OptionRun run;
        if (!RunRendered("Defaults", run, {}))
            return false;
        const std::string options = RecordStartingWith(run, "graphics options: ");
        const bool usualValidation = Contains(options, "validation off,") || Contains(options, "(debug build)");
        if (!usualValidation || !Contains(options, "adapter default"))
            return Fail("Without settings the start record must show the usual validation and the default adapter.");
        if (RecordStartingWith(run, "swap chain: 64x64, 2 buffers, tearing ").empty())
            return Fail("Without settings the swap chain must keep its two buffers.");
        if (Contains(RecordStartingWith(run, "swap chain: "), "waitable"))
            return Fail("Without settings the swap chain must not be waitable.");
        if (!RecordStartingWith(run, "graphics validation").empty())
            return Fail("Without a validation setting no validation records may be sent.");
        return true;
    }

    bool CheckSwapChainShape()
    {
        GraphicsDeviceDiagnosticsConfig graphics;
        graphics.allowTearing = false;
        graphics.swapChainBuffers = 3;
        graphics.waitableSwapChain = true;
        OptionRun run;
        if (!RunRendered("No tearing, three buffers, waitable", run, graphics))
            return false;
        const std::string swapChain = RecordStartingWith(run, "swap chain: ");
        if (!Contains(swapChain, "3 buffers") || !Contains(swapChain, "waitable (latency 1)"))
            return Fail("The start record must show three buffers and the waitable swap chain.");
        if (Contains(swapChain, "tearing allowed"))
            return Fail("Tearing must be off when the setting turns it off.");
        return true;
    }

    bool CheckAdapters()
    {
        GraphicsDeviceDiagnosticsConfig warp;
        warp.adapter = GraphicsAdapterRequest::Warp;
        OptionRun warpRun;
        if (!RunRendered("WARP", warpRun, warp))
            return false;
        if (!Contains(RecordStartingWith(warpRun, "graphics options: "), "adapter WARP"))
            return Fail("The start record must name the WARP request.");
        if (!Contains(RecordStartingWith(warpRun, "graphics adapter: "), "vendor 1414 device 008C"))
            return Fail("A WARP request must create the device on Microsoft's software adapter.");

        GraphicsDeviceDiagnosticsConfig missing;
        missing.adapter = GraphicsAdapterRequest::Index;
        missing.adapterIndex = 99;
        OptionRun missingRun;
        if (!RunRendered("Missing adapter index", missingRun, missing))
            return false;
        if (!Contains(RecordStartingWith(missingRun, "graphics options: "), "adapter index 99 (not found, default used)"))
            return Fail("A missing adapter must fall back to the default and say so.");
        return true;
    }

    // Validation is process-wide, so these runs come last.
    bool CheckValidation(const GraphicsValidation level, const char *title)
    {
        GraphicsDeviceDiagnosticsConfig graphics;
        graphics.validation = level;
        // The validated run also uses the other switches, so their use is checked too.
        graphics.allowTearing = false;
        graphics.swapChainBuffers = 3;
        graphics.waitableSwapChain = true;
        OptionRun run;
        if (!RunRendered(title, run, graphics))
            return false;
        const std::string options = RecordStartingWith(run, "graphics options: ");
        if (Contains(options, "unavailable"))
        {
            // Not a failure: the PC lacks Windows Graphics Tools and the run went on unchecked.
            std::cout << "  (debug layer not installed: validation could not be checked here)\n";
            return RecordStartingWith(run, "graphics validation").empty() || Fail("An unchecked run sends no validation records.");
        }
        const bool errors = !RecordStartingWith(run, "graphics validation error").empty();
        if (errors || !RecordStartingWith(run, "graphics validation corruption").empty())
            return Fail("A normal run must leave no validation errors or corruption.");
        const std::string summary = RecordStartingWith(run, "graphics validation summary: ");
        if (!summary.starts_with("graphics validation summary: 0 corruptions, 0 errors, "))
            return Fail("The run must end with a validation summary that counts no errors.");
        if (RecordStartingWith(run, "graphics live object: ").empty())
            return Fail("The summary must come with the live-object report (at least the device itself).");
        return true;
    }
} // namespace

int TestGraphicsOptions()
{
    const bool switches = CheckDefaults() && CheckSwapChainShape() && CheckAdapters();
    if (!switches)
        return EXIT_FAILURE;
    if (!CheckValidation(GraphicsValidation::DebugLayer, "Debug layer") ||
        !CheckValidation(GraphicsValidation::GpuBased, "GPU-based validation"))
        return EXIT_FAILURE;
    std::cout << "Graphics options passed: defaults, tearing off, three buffers, waitable swap chain, WARP, "
                 "adapter fallback, debug layer and GPU-based validation without errors.\n";
    return EXIT_SUCCESS;
}
