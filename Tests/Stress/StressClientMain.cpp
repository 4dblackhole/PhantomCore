// PhantomCore.StressClient: a client that only draws, for finding what a graphics device cannot take.
//
// It shows a window, fills a screen with one kind of primitive in many small draws, and runs until the time is
// up or the graphics device is lost. It is not part of the verification: a run may hang the GPU on purpose.
//
//   PhantomCore.StressClient --out <folder> [--kind text|image|mesh] [--draws N] [--glyphs N]
//       [--rate display|double|unlimited] [--hz N] [--seconds N] [--width N] [--height N] [--fill N] [--swap-ms N] [--hidden]
//
// --kind text   N labels of --glyphs letters, each followed by a plain rectangle, so no two labels share a draw.
// --kind image  N small images, each followed by a plain rectangle.
// --kind mesh   N quads, each with its own material, so every quad is its own draw.
// The folder gets engine.log (the engine's records and one line of statistics a second), result.txt, and
// loss.txt when the device was lost.
#include "PhantomCore.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace PhantomCore;

namespace
{
    struct Options
    {
        std::filesystem::path out{"stress-out"};
        std::string kind{"text"};
        std::size_t draws{120};
        std::size_t glyphs{3};
        // Full-screen translucent images under everything: the fill load of a game's backgrounds.
        std::size_t fill{};
        // Milliseconds between swaps of one image (released, made again), as a song list does; 0 for none.
        double swapMilliseconds{};
        RenderRateMode rate{RenderRateMode::DisplayDouble};
        double hz{};
        double seconds{120.0};
        std::uint32_t width{1600};
        std::uint32_t height{900};
        bool hidden{};
    };

    struct RunState
    {
        bool lost{};
        std::uint64_t frames{};
        double seconds{};
    };

    class Sink final : public IEngineDiagnosticSink
    {
      public:
        Sink(const Options &options, RunState &state) : options_(options), state_(state) {}
        void OnEngineRecord(const std::string_view line) noexcept override { Write("engine.log", line); }
        void OnGraphicsDeviceLost(const GraphicsDeviceLossReport &report) noexcept override
        {
            try
            {
                state_.lost = true;
                std::ofstream text(options_.out / "loss.txt");
                text << report.operation << " failed, reason " << report.removedReasonName << "\nframe "
                     << report.renderIndex << ", " << report.lastFrameDrawCalls << " draws in the last frame\n";
                for (const auto &line : report.extendedData)
                    text << line << '\n';
            }
            catch (...)
            {
            }
        }
        FatalErrorPresentation OnFatalError(const std::string_view message) noexcept override
        {
            Write("engine.log", std::string("FATAL ") + std::string(message));
            return FatalErrorPresentation::Handled;
        }
        void OnMainLoopStalled(const MainLoopStallReport &) noexcept override {}
        void OnMainLoopResumed(double) noexcept override {}

        void Write(const char *file, const std::string_view line) const noexcept
        {
            try
            {
                std::ofstream(options_.out / file, std::ios::app) << line << '\n';
            }
            catch (...)
            {
            }
        }

      private:
        const Options &options_;
        RunState &state_;
    };

    class Client final : public IGameClient
    {
      public:
        Client(const Options &options, RunState &state, Sink &sink) : options_(options), state_(state), sink_(sink) {}

        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.windowTitle = L"PhantomCore stress client";
            config.showWindow = !options_.hidden;
            config.windowWidth = options_.width;
            config.windowHeight = options_.height;
            config.windowMode = WindowMode::Borderless;
            config.renderRateMode = options_.rate;
            config.renderRateOverrideHz = options_.hz;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            config.clearColor = {0.05F, 0.05F, 0.08F, 1.0F};
            config.diagnostics.sink = &sink_;
            config.diagnostics.graphicsDeviceRemovedData = true;
            return config;
        }

        void Initialize(const EngineServices &services) override
        {
            const float width = static_cast<float>(options_.width), height = static_cast<float>(options_.height);
            const std::size_t columns = 16;
            const float cellWidth = width / static_cast<float>(columns), cellHeight = 22.0F;
            const std::vector<std::uint8_t> white(8 * 8 * 4, 255);
            ImageHandle image;
            if (options_.kind == "image")
                image = services.visual2DRendering.CreatePixelImage(8, 8, white);
            if (options_.fill != 0)
            {
                std::vector<std::uint8_t> pixels(1024 * 1024 * 4);
                for (std::size_t index = 0; index < pixels.size(); ++index)
                    pixels[index] = static_cast<std::uint8_t>((index * 31 + index / 4096) & 0xFF);
                const ImageHandle background = services.visual2DRendering.CreatePixelImage(1024, 1024, pixels);
                for (std::size_t index = 0; index < options_.fill; ++index)
                {
                    auto &layer = CreateSprite(canvas_.Root(), {-width / 2, -height / 2, width, height}, background);
                    layer.GetComponent<SpriteVisualComponent>()->SetTint({1.0F, 1.0F, 1.0F, 0.25F});
                }
            }
            rendering_ = &services.visual2DRendering;
            if (options_.swapMilliseconds > 0.0)
            {
                swapPixels_.assign(512 * 512 * 4, 200);
                swapImage_ = rendering_->CreatePixelImage(512, 512, swapPixels_);
                swapSprite_ = &CreateSprite(canvas_.Root(), {-width / 2, -height / 2, 256.0F, 256.0F}, swapImage_);
            }
            const TextFont font{TextFontSource::System, L"Segoe UI"};
            for (std::size_t index = 0; index < options_.draws && options_.kind != "mesh"; ++index)
            {
                const float x = -width / 2 + static_cast<float>(index % columns) * cellWidth;
                const float y = -height / 2 + static_cast<float>((index / columns) % 40) * cellHeight;
                if (options_.kind == "text")
                {
                    std::wstring text;
                    for (std::size_t glyph = 0; glyph < options_.glyphs; ++glyph)
                        text.push_back(static_cast<wchar_t>(L'A' + (index + glyph) % 26));
                    auto &label = CreateLabel(canvas_.Root(), {x, y, cellWidth - 24.0F, cellHeight}, std::move(text));
                    auto &component = *label.GetComponent<TextVisualComponent>();
                    component.SetFont(font);
                    component.SetFontSize(14.0F);
                    component.SetTextColor({1, 1, 1, 1});
                }
                else
                {
                    auto &sprite = CreateSprite(canvas_.Root(), {x, y, 20.0F, 20.0F}, image);
                    sprite.GetComponent<SpriteVisualComponent>()->SetTint({1.0F, 0.8F, 0.4F, 0.9F});
                }
                // The plain rectangle between two of them ends the draw of the first.
                auto &separator = CreateSprite(canvas_.Root(), {x + cellWidth - 22.0F, y, 18.0F, 18.0F}, {});
                separator.GetComponent<SpriteVisualComponent>()->SetTint({0.2F, 0.4F, 0.8F, 0.6F});
            }
            if (options_.kind == "mesh")
            {
                mesh_ = services.meshRendering.CreateMesh<VertexPositionColor>(RectangleShape{});
                for (std::size_t index = 0; index < options_.draws; ++index)
                    materials_.push_back(services.meshRendering.CreateMaterial(BuiltInMaterial::UnlitVertexColor));
            }
        }

        bool Update(const UpdateContext &context) override
        {
            state_.seconds = context.totalSeconds;
            if (context.performance.hasMeasurement && context.performance.measurementIndex != measurement_)
            {
                measurement_ = context.performance.measurementIndex;
                sink_.Write("engine.log", "  " + std::to_string(static_cast<int>(context.totalSeconds)) + "s fps " +
                                              std::to_string(context.performance.framesPerSecond) + " draws " +
                                              std::to_string(context.performance.drawCallsPerFrame) + "/" +
                                              std::to_string(context.performance.peakDrawCallsPerFrame));
            }
            if (swapSprite_ != nullptr && context.totalSeconds >= nextSwap_)
            {
                nextSwap_ = context.totalSeconds + options_.swapMilliseconds / 1000.0;
                rendering_->ReleaseImage(swapImage_);
                swapPixels_[swaps_++ % swapPixels_.size()] ^= 0x55;
                swapImage_ = rendering_->CreatePixelImage(512, 512, swapPixels_);
                swapSprite_->GetComponent<SpriteVisualComponent>()->SetImage(swapImage_);
            }
            return context.totalSeconds < options_.seconds;
        }

        void Render(const RenderContext &context) override
        {
            ++state_.frames;
            if (options_.kind == "mesh")
            {
                const Float4x4 viewProjection = OrthographicLHMatrix(32.0F, 18.0F, 0.1F, 10.0F);
                for (std::size_t index = 0; index < materials_.size(); ++index)
                {
                    const float x = -15.0F + static_cast<float>(index % 16) * 2.0F;
                    const float y = -8.0F + static_cast<float>((index / 16) % 9) * 2.0F;
                    context.meshRendering->Submit(mesh_, materials_[index], TranslationMatrix(x, y, 1.0F), viewProjection,
                                                  {1, 1, 1, 1}, {1, 1}, {}, NoTextureIndex);
                }
                return;
            }
            canvas_.SetViewportSize({static_cast<float>(context.width), static_cast<float>(context.height)});
            context.visual2DRendering->SubmitScreen(canvas_, context, {}, 0);
        }

        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override {}

      private:
        const Options &options_;
        RunState &state_;
        Sink &sink_;
        Visual2DCanvas canvas_{{static_cast<float>(options_.width), static_cast<float>(options_.height)}};
        GpuMeshHandle mesh_;
        std::vector<MaterialInstanceHandle> materials_;
        std::uint64_t measurement_{};
        Visual2DRenderSystem *rendering_{};
        Visual2DNode *swapSprite_{};
        ImageHandle swapImage_;
        std::vector<std::uint8_t> swapPixels_;
        std::size_t swaps_{};
        double nextSwap_{1.0};
    };

    [[nodiscard]] Options Parse(const int argc, char **argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view name = argv[index];
            if (name == "--hidden")
            {
                options.hidden = true;
                continue;
            }
            if (index + 1 >= argc)
                break;
            const std::string value = argv[++index];
            if (name == "--out")
                options.out = value;
            else if (name == "--kind")
                options.kind = value;
            else if (name == "--draws")
                options.draws = static_cast<std::size_t>(std::atoll(value.c_str()));
            else if (name == "--glyphs")
                options.glyphs = static_cast<std::size_t>(std::atoll(value.c_str()));
            else if (name == "--fill")
                options.fill = static_cast<std::size_t>(std::atoll(value.c_str()));
            else if (name == "--swap-ms")
                options.swapMilliseconds = std::atof(value.c_str());
            else if (name == "--hz")
                options.hz = std::atof(value.c_str());
            else if (name == "--seconds")
                options.seconds = std::atof(value.c_str());
            else if (name == "--width")
                options.width = static_cast<std::uint32_t>(std::atoi(value.c_str()));
            else if (name == "--height")
                options.height = static_cast<std::uint32_t>(std::atoi(value.c_str()));
            else if (name == "--rate" && value == "display")
                options.rate = RenderRateMode::Display;
            else if (name == "--rate" && value == "unlimited")
                options.rate = RenderRateMode::Unlimited;
        }
        return options;
    }
} // namespace

int main(const int argc, char **argv)
{
    const Options options = Parse(argc, argv);
    std::filesystem::create_directories(options.out);
    RunState state;
    Sink sink(options, state);
    int result = -1;
    try
    {
        result = Run(std::make_unique<Client>(options, state, sink));
    }
    catch (const std::exception &error)
    {
        sink.Write("engine.log", std::string("exception: ") + error.what());
    }
    const std::string line = "kind " + options.kind + ", draws asked " + std::to_string(options.draws) + ", frames " +
                             std::to_string(state.frames) + ", seconds " +
                             std::to_string(static_cast<int>(state.seconds)) + ", device lost " +
                             (state.lost ? "yes" : "no") + ", run result " + std::to_string(result);
    sink.Write("result.txt", line);
    std::cout << line << '\n';
    return state.lost ? 2 : 0;
}
