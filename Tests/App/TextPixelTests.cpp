#include "TextPixelTests.h"
#include "PhantomCore.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>

namespace
{
    constexpr std::uint32_t Width = 300, Height = 120;
    struct PixelFixture
    {
        FontHandle font;
        std::shared_future<RgbaImage> capture;

        bool VerifyPixels()
        {
            const auto& image = capture.get();
            if (image.width < Width || image.height < Height ||
                image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4)
                throw std::runtime_error("Text pixel fixture received an invalid frame image.");
            const auto channel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t color)
            {
                return image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4 + color] / 255.0;
            };
            constexpr double background = 15.0 / 255;
            double error = 0;
            std::size_t covered = 0;
            for (std::uint32_t y = 0; y < 80; ++y)
                for (std::uint32_t x = 0; x < 80; ++x)
                {
                    const double a =
                        std::clamp((channel(110 + x, 10 + y, 0) - background) / (1 - background), 0.0, 1.0);
                    const double b =
                        std::clamp((channel(210 + x, 10 + y, 0) - background) / (1 - background), 0.0, 1.0);
                    if (a > .5 && b > .5)
                        ++covered;
                    const double behind = background * (1 - a) * (1 - b);
                    const std::array<double, 3> expected{b * (1 - a) + behind, behind, a + behind};
                    for (std::uint32_t color = 0; color < 3; ++color)
                        error += std::abs(channel(10 + x, 10 + y, color) - expected[color]) * 255;
                }
            error /= 80 * 80 * 3;
            std::cout << "Text alpha paint order: byte error=" << error << ", overlapping pixels=" << covered << '\n';
            return error < 1 && covered > 100;
        }
    };
    class PixelClient final : public IGameClient
    {
      public:
        explicit PixelClient(PixelFixture &fixture) : fixture_(fixture)
        {
        }
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = Width;
            config.windowHeight = Height;
            config.clearColor = {.06F, .06F, .06F, 1};
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices &services) override
        {
            fixture_.font = services.textRendering.LoadSystemFont(L"Segoe UI");
        }
        bool Update(const UpdateContext &) override
        {
            return !rendered_;
        }
        void Render(const RenderContext &context) override
        {
            auto& text = *context.textRendering;
            TextDrawCommand draw;
            draw.style.font = fixture_.font;
            draw.style.fontSizePixels = 64;
            draw.layoutSizePixels = {80, 80};
            draw.positionPixels = {0, 1000};
            text.Submit(L"A", draw); // A occupies an early atlas page.
            std::wstring bank;
            for (std::uint32_t index = 0; index < 1000; ++index)
                bank.push_back(static_cast<wchar_t>(0xAC00U + index));
            text.Submit(bank, draw);
            draw.positionPixels = {10, 10};
            draw.style.color = {1, 0, 0, 1};
            text.Submit(L"B", draw); // B occupies a later page, but paints first.
            draw.style.color = {0, 0, 1, 1};
            text.Submit(L"A", draw);
            draw.style.color = {1, 1, 1, 1};
            draw.positionPixels = {110, 10};
            text.Submit(L"A", draw);
            draw.positionPixels = {210, 10};
            text.Submit(L"B", draw);
            fixture_.capture = context.ReadImage();
            rendered_ = true;
        }
        void OnResize(std::uint32_t, std::uint32_t) override
        {
        }
        void Shutdown() noexcept override
        {
        }

      private:
        PixelFixture &fixture_;
        bool rendered_{};
    };
} // namespace

int TestTextPixelOrder()
{
    PixelFixture fixture;
    if (Run(std::make_unique<PixelClient>(fixture)) != 0)
        return 1;
    return fixture.VerifyPixels() ? 0 : 1;
}
