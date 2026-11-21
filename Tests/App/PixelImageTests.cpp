#include "PixelImageTests.h"
#include "PhantomCore.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace PhantomCore;

namespace
{
    constexpr std::uint32_t Extent = 128;
    constexpr unsigned Frames = 13;

    std::vector<std::uint8_t> Pixels(std::initializer_list<std::array<std::uint8_t, 3>> colors)
    {
        std::vector<std::uint8_t> pixels;
        for (const auto &color : colors)
            pixels.insert(pixels.end(), {color[0], color[1], color[2], 255});
        return pixels;
    }

    // Draws a pixel image over the whole Canvas and walks it through creation, capacity, release and slot reuse.
    class PixelImageClient final : public IGameClient
    {
    public:
        PixelImageClient(std::vector<std::string> &failures, std::shared_future<RgbaImage> &first,
                         std::shared_future<RgbaImage> &reused)
            : failures_(failures), first_(first), reused_(reused) {}
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = Extent;
            config.windowHeight = Extent;
            config.autoExitAfterRenderedFrames = Frames;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            config.clearColor = {0, 0, 0, 1};
            return config;
        }
        void Initialize(const EngineServices &services) override
        {
            rendering_ = &services.visual2DRendering;
            Expect([&] { static_cast<void>(rendering_->CreatePixelImage(0, 1, {})); }, true, "empty size");
            const auto short_ = Pixels({{255, 0, 0}});
            Expect([&] { static_cast<void>(rendering_->CreatePixelImage(2, 1, short_)); }, true, "wrong byte count");
            // Left half red, right half blue.
            const auto twoColors = Pixels({{255, 0, 0}, {0, 0, 255}});
            firstImage_ = rendering_->CreatePixelImage(2, 1, twoColors);
            if (rendering_->GetImageSize(firstImage_).width != 2)
                failures_.push_back("A pixel image must report its pixel size.");
            sprite_ = &CreateSprite(canvas_.Root(), {-64, -64, 128, 128}, firstImage_);
            sprite_->GetComponent<SpriteVisualComponent>()->SetTint({1, 1, 1, 1});
        }
        bool Update(const UpdateContext &) override { return true; }
        void Render(const RenderContext &context) override
        {
            ++frame_;
            const auto green = Pixels({{0, 255, 0}});
            if (frame_ == 2)
            {
                // Fill every slot, then release one: it is not free while the frame ring may still sample it.
                while (images_.size() + 1 < MaxPixelImages)
                    images_.push_back(rendering_->CreatePixelImage(1, 1, green));
                Expect([&] { static_cast<void>(rendering_->CreatePixelImage(1, 1, green)); }, true, "capacity");
                rendering_->ReleaseImage(images_.back());
                images_.pop_back();
            }
            if (frame_ == 3)
                Expect([&] { static_cast<void>(rendering_->CreatePixelImage(1, 1, green)); }, true, "retired slot reuse");
            if (frame_ == 6)
            {
                Expect([&] { images_.push_back(rendering_->CreatePixelImage(1, 1, green)); }, false, "slot after the ring");
                // The sprite keeps naming the released image and draws nothing until it gets the next one.
                rendering_->ReleaseImage(firstImage_);
                rendering_->ReleaseImage(firstImage_);
            }
            if (frame_ == 10)
            {
                const auto yellow = Pixels({{255, 255, 0}});
                const auto reused = rendering_->CreatePixelImage(1, 1, yellow);
                sprite_->GetComponent<SpriteVisualComponent>()->SetImage(reused);
            }
            canvas_.SetViewportSize({static_cast<float>(context.width), static_cast<float>(context.height)});
            context.visual2DRendering->SubmitScreen(canvas_, context, {}, 0);
            if (frame_ == 1)
                first_ = context.ReadImage();
            if (frame_ == 12)
                reused_ = context.ReadImage();
        }
        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override {}

    private:
        template <class Action> void Expect(Action action, bool throws, const char *name)
        {
            bool threw = false;
            try
            {
                action();
            }
            catch (const std::exception &)
            {
                threw = true;
            }
            if (threw != throws)
                failures_.push_back(std::string(name) + (throws ? ": expected a rejection" : ": unexpected rejection"));
        }
        std::vector<std::string> &failures_;
        std::shared_future<RgbaImage> &first_;
        std::shared_future<RgbaImage> &reused_;
        Visual2DRenderSystem *rendering_{};
        Visual2DCanvas canvas_{{Extent, Extent}};
        Visual2DNode *sprite_{};
        ImageHandle firstImage_;
        std::vector<ImageHandle> images_;
        unsigned frame_{};
    };

    std::array<int, 3> Sample(const RgbaImage &image, std::uint32_t x, std::uint32_t y)
    {
        const std::size_t pixel = (static_cast<std::size_t>(y) * image.width + x) * 4;
        return {image.pixels.at(pixel), image.pixels.at(pixel + 1), image.pixels.at(pixel + 2)};
    }
} // namespace

int TestPixelImages()
{
    try
    {
        std::vector<std::string> failures;
        std::shared_future<RgbaImage> first, reused;
        std::packaged_task<int()> run([&] {
            GraphicsDiagnostics diagnostics;
            const int result =
                RunWithGraphicsDiagnostics(std::make_unique<PixelImageClient>(failures, first, reused), diagnostics);
            for (const auto &message : diagnostics.messages)
                failures.push_back("GPU: " + message.description);
            return result != 0 || diagnostics.status == GraphicsDiagnostics::Status::ReadFailed ? 1 : 0;
        });
        auto exitCode = run.get_future();
        std::jthread(std::move(run)).join();
        if (exitCode.get() != 0)
            failures.push_back("The engine run failed.");
        const auto check = [&failures](const RgbaImage &image, std::uint32_t x, std::array<int, 3> expected, const char *name) {
            const auto actual = Sample(image, x, Extent / 2);
            for (std::size_t channel = 0; channel < 3; ++channel)
                if (std::abs(actual[channel] - expected[channel]) > 3)
                {
                    failures.push_back(std::string(name) + ": " + std::to_string(actual[0]) + ',' +
                                       std::to_string(actual[1]) + ',' + std::to_string(actual[2]));
                    return;
                }
        };
        if (first.valid() && reused.valid())
        {
            check(first.get(), 16, {255, 0, 0}, "left pixel of the 2x1 image");
            check(first.get(), Extent - 16, {0, 0, 255}, "right pixel of the 2x1 image");
            check(reused.get(), Extent / 2, {255, 255, 0}, "new pixels in a reused slot");
        }
        else
            failures.push_back("No captures were taken.");
        for (const auto &failure : failures)
            std::cerr << "Pixel image test: " << failure << '\n';
        std::cout << (failures.empty() ? "Pixel image tests passed.\n" : "Pixel image tests failed.\n");
        return failures.empty() ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Pixel image test failed: " << error.what() << '\n';
        return 1;
    }
}
