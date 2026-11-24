#include "MainLoopWatchTests.h"
#include "PhantomCore.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

using namespace PhantomCore;

namespace
{
    constexpr double StallSeconds = 0.5;
    constexpr auto HoldTime = std::chrono::milliseconds(1700);
    // Well under the limit even on a busy PC: a slow frame, not a stall.
    constexpr auto BriefHoldTime = std::chrono::milliseconds(150);

    // The stall calls arrive on the engine's watch thread, so everything they
    // write is atomic.
    struct WatchLog
    {
        std::atomic<int> stalls{};
        std::atomic<int> resumes{};
        std::atomic<double> stalledSeconds{};
        std::atomic<double> resumedSeconds{};
        std::atomic<std::uint32_t> reportedThreadId{};
        std::atomic<std::uint64_t> reportedPasses{};
        std::atomic<std::uint32_t> callerThreadId{};
        std::uint32_t loopThreadId{};
    };

    class WatchSink final : public IEngineDiagnosticSink
    {
      public:
        explicit WatchSink(WatchLog &log) : log_(log)
        {
        }
        void OnEngineRecord(std::string_view) noexcept override
        {
        }
        void OnGraphicsDeviceLost(const GraphicsDeviceLossReport &) noexcept override
        {
        }
        FatalErrorPresentation OnFatalError(std::string_view) noexcept override
        {
            return FatalErrorPresentation::Handled;
        }
        void OnMainLoopStalled(const MainLoopStallReport &report) noexcept override
        {
            log_.stalledSeconds = report.stalledSeconds;
            log_.reportedThreadId = report.mainThreadId;
            log_.reportedPasses = report.loopPasses;
            log_.callerThreadId = GetCurrentThreadId();
            ++log_.stalls;
        }
        void OnMainLoopResumed(const double stalledSeconds) noexcept override
        {
            log_.resumedSeconds = stalledSeconds;
            ++log_.resumes;
        }

      private:
        WatchLog &log_;
    };

    enum class Hold
    {
        None,         // the loop keeps running
        Brief,        // one Update takes longer than a frame but less than the limit
        Stuck,        // one Update sleeps: nothing answers the window
        WindowLoop,   // one Update runs a message loop, as moving the window or a dialog does
        SlowShutdown, // the loop keeps running; Shutdown, after the loop, takes longer than the limit
    };

    // Runs for a moment, holds one Update for HoldTime in the chosen way, then
    // runs a little longer so a resume can be seen.
    class HoldingClient final : public IGameClient
    {
      public:
        HoldingClient(WatchLog &log, WatchSink &sink, const Hold hold) : log_(log), sink_(sink), hold_(hold)
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
            config.diagnostics.mainLoopStallSeconds = StallSeconds;
            return config;
        }
        void Initialize(const EngineServices &) override
        {
            log_.loopThreadId = GetCurrentThreadId();
            start_ = std::chrono::steady_clock::now();
        }
        bool Update(const UpdateContext &) override
        {
            const auto elapsed = std::chrono::steady_clock::now() - start_;
            if (!held_ && elapsed > std::chrono::milliseconds(300))
            {
                held_ = true;
                HoldOnce();
                heldUntil_ = std::chrono::steady_clock::now();
            }
            const auto sinceHold = std::chrono::steady_clock::now() - heldUntil_;
            const bool finished = held_ && sinceHold > std::chrono::milliseconds(800);
            return !finished && elapsed < std::chrono::seconds(20);
        }
        void Render(const RenderContext &) override
        {
        }
        void OnResize(std::uint32_t, std::uint32_t) override
        {
        }
        void Shutdown() noexcept override
        {
            // The watch belongs to the loop. What follows the loop is not its business.
            if (hold_ == Hold::SlowShutdown)
                std::this_thread::sleep_for(HoldTime);
        }

      private:
        void HoldOnce() const
        {
            if (hold_ == Hold::Brief)
                std::this_thread::sleep_for(BriefHoldTime);
            if (hold_ == Hold::Stuck)
                std::this_thread::sleep_for(HoldTime);
            if (hold_ != Hold::WindowLoop)
                return;
            const auto until = std::chrono::steady_clock::now() + HoldTime;
            while (std::chrono::steady_clock::now() < until)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    DispatchMessageW(&message);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        WatchLog &log_;
        WatchSink &sink_;
        Hold hold_;
        bool held_{};
        std::chrono::steady_clock::time_point start_{};
        std::chrono::steady_clock::time_point heldUntil_{};
    };

    bool RunHolding(WatchLog &log, const Hold hold)
    {
        WatchSink sink(log);
        int result = EXIT_FAILURE;
        std::jthread engine([&] { result = Run(std::make_unique<HoldingClient>(log, sink, hold)); });
        engine.join();
        return result == EXIT_SUCCESS;
    }

    bool Fail(const char *message)
    {
        std::cerr << message << '\n';
        return false;
    }

    bool CheckQuiet(const Hold hold, const char *message)
    {
        WatchLog log;
        if (!RunHolding(log, hold))
            return Fail("A watched run must still succeed.");
        if (log.stalls != 0 || log.resumes != 0)
            return Fail(message);
        return true;
    }

    bool CheckStuck()
    {
        WatchLog log;
        if (!RunHolding(log, Hold::Stuck))
            return Fail("A run whose loop was stuck for a while must still succeed.");
        if (log.stalls != 1 || log.resumes != 1)
            return Fail("One stuck Update must be reported as one stall and one resume.");
        const bool timed = log.stalledSeconds >= StallSeconds && log.resumedSeconds >= log.stalledSeconds;
        if (!timed || log.reportedPasses == 0)
            return Fail("The stall report must carry how long the loop stood still and the passes before it.");
        const bool rightThread = log.reportedThreadId == log.loopThreadId && log.callerThreadId != log.loopThreadId;
        if (!rightThread)
            return Fail("The stall must name the loop thread and be reported from another thread.");
        std::cout << "Stuck Update: reported after " << log.stalledSeconds << " s, resumed after " << log.resumedSeconds
                  << " s, " << log.reportedPasses << " passes before it.\n";
        return true;
    }
} // namespace

int TestMainLoopWatch()
{
    if (!CheckQuiet(Hold::None, "A loop that keeps running must not be reported."))
        return EXIT_FAILURE;
    if (!CheckQuiet(Hold::Brief, "An Update shorter than the limit must not be reported."))
        return EXIT_FAILURE;
    if (!CheckQuiet(Hold::WindowLoop, "A loop held by a window message loop must not be reported."))
        return EXIT_FAILURE;
    if (!CheckQuiet(Hold::SlowShutdown, "A slow Shutdown after the loop ended must not be reported."))
        return EXIT_FAILURE;
    if (!CheckStuck())
        return EXIT_FAILURE;
    std::cout << "Main loop watch passed: a running loop, a slow frame, a window loop and a slow Shutdown stay quiet; "
                 "a stuck Update is reported once.\n";
    return EXIT_SUCCESS;
}
