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
