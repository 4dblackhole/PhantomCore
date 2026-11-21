#include "HeadlessRunTests.h"
#include "PhantomCore.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace PhantomCore;

namespace
{
    class FailingHiddenClient final : public IGameClient
    {
      public:
        explicit FailingHiddenClient(bool &shutdown) : shutdown_(shutdown)
        {
        }
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices &) override
        {
        }
        bool Update(const UpdateContext &) override
        {
            throw std::runtime_error("Expected hidden Update failure.");
        }
        void Render(const RenderContext &) override
        {
        }
        void OnResize(std::uint32_t, std::uint32_t) override
        {
        }
        void Shutdown() noexcept override
        {
            shutdown_ = true;
        }

      private:
        bool &shutdown_;
    };
} // namespace

namespace
{
    // Renders one screen Canvas whose two rectangles share one batch, then
    // stops after the first one-second performance sample.
    class DrawStatisticsClient final : public IGameClient
    {
      public:
        explicit DrawStatisticsClient(PerformanceStatistics &sample) : sample_(sample)
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
            return config;
        }
        void Initialize(const EngineServices &) override
        {
            static_cast<void>(CreatePanel(canvas_.Root(), {-32, -32, 32, 32}));
            static_cast<void>(CreatePanel(canvas_.Root(), {0, 0, 32, 32}));
        }
        bool Update(const UpdateContext &context) override
        {
            if (context.performance.hasMeasurement)
                sample_ = context.performance;
            return !context.performance.hasMeasurement && context.totalSeconds < 10.0;
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
        }

      private:
        PerformanceStatistics &sample_;
        Visual2DCanvas canvas_{{64, 64}};
    };
} // namespace

namespace
{
    constexpr double RenderRateBaseHz = 100.0;
    constexpr double RenderRatePhaseSeconds = 0.5;
    constexpr RenderRateMode RenderRatePhases[]{RenderRateMode::Display, RenderRateMode::DisplayDouble,
                                                RenderRateMode::Unlimited};
    constexpr std::size_t RenderRatePhaseCount = sizeof(RenderRatePhases) / sizeof(RenderRatePhases[0]);

    struct RenderRateCounts
    {
        bool hadControl{};
        std::uint64_t updates[RenderRatePhaseCount]{};
        std::uint64_t renders[RenderRatePhaseCount]{};
    };

    // Runs each render rate mode for half a second on a 100 Hz base rate,
    // switching through the engine service, and counts Update and Render
    // calls per mode.
    class RenderRateClient final : public IGameClient
    {
      public:
        explicit RenderRateClient(RenderRateCounts &counts) : counts_(counts)
        {
        }
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = 64;
            config.windowHeight = 64;
            config.renderRateOverrideHz = RenderRateBaseHz;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices &services) override
        {
            control_ = services.renderRateControl;
            counts_.hadControl = control_ != nullptr;
        }
        bool Update(const UpdateContext &context) override
        {
            if (!control_)
                return false;
            if (!started_)
            {
                started_ = true;
                startSeconds_ = context.totalSeconds;
            }
            const auto phase =
                static_cast<std::size_t>((context.totalSeconds - startSeconds_) / RenderRatePhaseSeconds);
            if (phase >= RenderRatePhaseCount)
                return false;
            // The new mode applies from the next loop iteration, so the
            // first Render of a phase can still belong to the previous mode.
            control_->SetMode(RenderRatePhases[phase]);
            phase_ = phase;
            ++counts_.updates[phase];
            return true;
        }
        void Render(const RenderContext &) override
        {
            ++counts_.renders[phase_];
        }
        void OnResize(std::uint32_t, std::uint32_t) override
        {
        }
        void Shutdown() noexcept override
        {
        }

      private:
        RenderRateCounts &counts_;
        IRenderRateControl *control_{};
        bool started_{};
        double startSeconds_{};
        std::size_t phase_{};
    };

    bool InRange(const std::uint64_t value, const std::uint64_t low, const std::uint64_t high)
    {
        return value >= low && value <= high;
    }
} // namespace

int TestRenderRateModes()
{
    RenderRateCounts counts;
    if (Run(std::make_unique<RenderRateClient>(counts)) != EXIT_SUCCESS || !counts.hadControl)
    {
        std::cerr << "Run must provide the render rate control and finish the mode sequence.\n";
        return EXIT_FAILURE;
    }
    // Upper bounds are the exact frame budget of half a second plus the
    // frames at the phase edges; lower bounds leave room for a busy PC.
    const auto budget = static_cast<std::uint64_t>(RenderRateBaseHz * RenderRatePhaseSeconds);
    const bool displayOk = InRange(counts.renders[0], budget / 2, budget + 2);
    const bool doubleOk = InRange(counts.renders[1], budget + budget / 5, budget * 2 + 2);
    const bool unlimitedOk = InRange(counts.renders[2] + 2, counts.updates[2], counts.updates[2] + 4);
    const bool limitedBelowUpdates = counts.renders[0] < counts.updates[0] && counts.renders[1] < counts.updates[1];
    const bool limitedOk = displayOk && doubleOk && limitedBelowUpdates;
    if (!limitedOk || !unlimitedOk)
    {
        std::cerr << "Render rate modes rendered " << counts.renders[0] << " / " << counts.renders[1] << " / "
                  << counts.renders[2] << " frames for " << counts.updates[0] << " / " << counts.updates[1] << " / "
                  << counts.updates[2] << " updates (x1 / x2 / unlimited at 100 Hz, half a second each).\n";
        return EXIT_FAILURE;
    }
    std::cout << "Render rate modes rendered " << counts.renders[0] << " / " << counts.renders[1] << " / "
              << counts.renders[2] << " frames (x1 / x2 / unlimited at 100 Hz, half a second each).\n";
    return EXIT_SUCCESS;
}

int TestDrawStatistics()
{
    PerformanceStatistics sample;
    if (Run(std::make_unique<DrawStatisticsClient>(sample)) != EXIT_SUCCESS || !sample.hasMeasurement)
    {
        std::cerr << "The draw statistics run must publish one performance sample.\n";
        return EXIT_FAILURE;
    }
    if (sample.framesPerSecond == 0 || sample.drawCallsPerFrame != 1 || sample.peakDrawCallsPerFrame != 1)
    {
        std::cerr << "One batched rectangle Canvas must report one draw per frame: average "
                  << sample.drawCallsPerFrame << ", peak " << sample.peakDrawCallsPerFrame << ", fps "
                  << sample.framesPerSecond << ".\n";
        return EXIT_FAILURE;
    }
    std::cout << "Performance sample reports one batched draw per rendered frame.\n";
    return EXIT_SUCCESS;
}

int TestHeadlessRunError()
{
    bool shutdown = false;
    const auto result = Run(std::make_unique<FailingHiddenClient>(shutdown));
    if (result != EXIT_FAILURE || !shutdown)
    {
        std::cerr << "Hidden Run must fail without blocking and clean up the initialized Client.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Hidden fatal-error cleanup passed without a modal dialog.\n";
    return EXIT_SUCCESS;
}
