#include "HeadlessRunTests.h"
#include "PhantomCore.h"
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
