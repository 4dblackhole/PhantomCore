#include "ImageReadbackTests.h"
#include "PhantomCore.h"

#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace PhantomCore;

namespace
{
    constexpr std::array<Color, 5> Colors{{
        {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}, {0, 1, 1, 1}}};

    void Require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    struct CaptureState final
    {
        std::vector<std::shared_future<RgbaImage>> images;
        std::vector<std::shared_future<RgbaImage>> duplicates;
        std::uint32_t width{}, height{};
        bool completedBeforeShutdown{};
    };

    class CaptureClient final : public IGameClient
    {
    public:
        CaptureClient(CaptureState& state, bool fail) : state_(state), fail_(fail) {}

        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = 67; // Deliberately not an aligned GPU readback row width.
            config.windowHeight = 39;
            config.autoExitAfterRenderedFrames = Colors.size();
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices&) override
        {
            auto& panel = CreatePanel(canvas_.Root(), {-1000, -1000, 2000, 2000});
            panel_ = panel.GetComponent<SpriteVisualComponent>();
        }
        bool Update(const UpdateContext&) override { return true; }
        void Render(const RenderContext& context) override
        {
            state_.width = context.width;
            state_.height = context.height;
            state_.images.push_back(context.ReadImage());
            state_.duplicates.push_back(context.ReadImage());
            Require(state_.images.back().wait_for(std::chrono::seconds(0)) == std::future_status::timeout,
                    "A frame image must not complete before its frame is submitted.");
            if (fail_)
                throw std::runtime_error("Intentional failure after requesting a frame image.");

            // A request captures final frame output, including draws submitted after it.
            canvas_.SetViewportSize({static_cast<float>(context.width), static_cast<float>(context.height)});
            panel_->SetTint(Colors.at(frame_++));
            context.visual2DRendering->SubmitScreen(canvas_, context);
        }
        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override
        {
            state_.completedBeforeShutdown = true;
            for (const auto& image : state_.images)
                state_.completedBeforeShutdown &= image.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }

    private:
        CaptureState& state_;
        bool fail_{};
        Visual2DCanvas canvas_{{67, 39}};
        SpriteVisualComponent* panel_{};
        std::size_t frame_{};
    };
}

int TestImageReadback()
{
    try
    {
        bool rejected = false;
        try { (void)RenderContext{}.ReadImage(); }
        catch (const std::logic_error&) { rejected = true; }
        Require(rejected, "An inactive RenderContext must reject frame image requests.");

        CaptureState state;
        Require(Run(std::make_unique<CaptureClient>(state, false)) == 0, "Frame image run failed.");
        Require(state.completedBeforeShutdown && state.images.size() == Colors.size(),
                "All requested images must complete before Client shutdown.");
        for (std::size_t index = 0; index < state.images.size(); ++index)
        {
            const auto& image = state.images[index].get();
            Require(&image == &state.duplicates[index].get(), "Repeated requests must share the same frame image.");
            Require(image.width == state.width && image.height == state.height &&
                        image.pixels.size() == static_cast<std::size_t>(image.width) * image.height * 4,
                    "Frame images must contain tightly packed RGBA rows.");
            const auto color = Colors[index];
            const std::array<std::uint8_t, 4> expected{
                static_cast<std::uint8_t>(color.red * 255), static_cast<std::uint8_t>(color.green * 255),
                static_cast<std::uint8_t>(color.blue * 255), 255};
            for (std::size_t pixel = 0; pixel < image.pixels.size(); ++pixel)
                Require(image.pixels[pixel] == expected[pixel % 4],
                        "A completed frame must retain its own pixels after later frames and engine destruction.");
        }

        CaptureState cancelled;
        // A dedicated thread starts with a fresh Win32 message queue.
        std::packaged_task<int()> cancelledRun([&] {
            return Run(std::make_unique<CaptureClient>(cancelled, true));
        });
        auto exitCode = cancelledRun.get_future();
        std::jthread(std::move(cancelledRun)).join();
        Require(exitCode.get() != 0,
                "The intentional Render exception must fail the engine run.");
        Require(cancelled.completedBeforeShutdown && cancelled.images.size() == 1,
                "An unsubmitted image request must complete with failure before shutdown.");
        bool failed = false;
        try { (void)cancelled.images.front().get(); }
        catch (const std::exception&) { failed = true; }
        Require(failed, "An unsubmitted image must report failure instead of returning pixels.");
        std::cout << "Frame image readback: five frames, duplicate requests, lifetime and cancellation passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Frame image readback failed: " << error.what() << '\n';
        return 1;
    }
}
