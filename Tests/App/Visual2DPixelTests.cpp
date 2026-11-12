#include "Visual2DPixelTests.h"
#include "PhantomCore.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
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
    constexpr Color Blue{0, 0, 1, 1}, Red{1, 0, 0, 1}, Green{0, 1, 0, 1}, White{1, 1, 1, 1};
    enum class Primitive { Rectangle, Image, Text };
    struct Layer
    {
        Primitive primitive;
        Color color;
        unsigned canvas{};
    };
    struct PixelCase
    {
        const char* name;
        std::vector<Layer> layers;
        std::array<int, 3> expected;
        bool knownFailure{};
        bool textSample{};
        bool reverseCanvasSubmission{};
        bool world{};
        bool planeInFront{};
        bool screenOnWorld{};
        bool sameCanvasZ{};
        bool highCanvasZ{};
        bool directText{};
        bool discardCanvas{};
        unsigned frames{1};
    };
    Layer Rectangle(Color color, unsigned canvas = 0) { return {Primitive::Rectangle, color, canvas}; }
    Layer Image(Color color, unsigned canvas = 0) { return {Primitive::Image, color, canvas}; }
    Layer Text(unsigned canvas = 0) { return {Primitive::Text, White, canvas}; }

    class WhiteImageFile final
    {
    public:
        WhiteImageFile()
        {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("phantomcore-visual2d-pixels-" + std::to_string(nonce) + ".bmp");
            constexpr auto image = std::to_array<unsigned char>({
                0x42, 0x4d, 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40,
                0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
                0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0});
            static_assert(image.size() == 58);
            std::ofstream file(path, std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(reinterpret_cast<const char*>(image.data()), image.size());
        }
        ~WhiteImageFile()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
        std::filesystem::path path;
    };

    class PixelClient final : public IGameClient
    {
    public:
        PixelClient(const PixelCase& test, const std::filesystem::path& imagePath,
                    std::shared_future<RgbaImage>& capture)
            : test_(test), imagePath_(imagePath), capture_(capture) {}
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = Extent;
            config.windowHeight = Extent;
            config.autoExitAfterRenderedFrames = test_.frames;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            config.clearColor = {0, 0, 0, 1};
            return config;
        }
        void Initialize(const EngineServices& services) override
        {
            const TextFont font{TextFontSource::System, L"Segoe UI"};
            const auto image = services.visual2DRendering.LoadImageFile(imagePath_);
            if (test_.directText)
                directFont_ = services.textRendering.LoadSystemFont(L"Segoe UI");
            for (const auto& layer : test_.layers)
            {
                auto& root = canvases_.at(layer.canvas).Root();
                if (layer.primitive == Primitive::Text)
                {
                    auto& label = CreateLabel(root, {-48, -48, 96, 96}, L"A");
                    auto& text = *label.GetComponent<TextVisualComponent>();
                    text.SetFont(font);
                    text.SetFontSize(64);
                    text.SetTextColor(layer.color);
                }
                else
                {
                    auto& panel = CreateSprite(root, {-64, -64, 128, 128},
                                               layer.primitive == Primitive::Image ? image : ImageHandle{});
                    panel.GetComponent<SpriteVisualComponent>()->SetTint(layer.color);
                }
            }
            if (test_.world)
            {
                mesh_ = services.meshRendering.CreateMesh<VertexPositionColor>(RectangleShape{});
                material_ = services.meshRendering.CreateMaterial(BuiltInMaterial::UnlitVertexColor);
                camera_.SetOrthographic(2, 2, .1F, 10);
            }
        }
        bool Update(const UpdateContext&) override { return true; }
        void Render(const RenderContext& context) override
        {
            for (auto& canvas : canvases_)
                canvas.SetViewportSize({static_cast<float>(context.width), static_cast<float>(context.height)});
            if (test_.world)
                RenderWorld(context);
            else
            {
                const unsigned first = test_.reverseCanvasSubmission ? 1 : 0;
                const auto z = [&](unsigned index)
                {
                    return test_.sameCanvasZ ? 0 : index + (test_.highCanvasZ ? 1000 : 0);
                };
                context.visual2DRendering->SubmitScreen(canvases_[first], context, {}, z(first));
                context.visual2DRendering->SubmitScreen(canvases_[1 - first], context, {}, z(1 - first));
            }
            if (test_.discardCanvas)
            {
                for (auto& canvas : canvases_)
                {
                    std::vector<NodeId> nodes;
                    for (const auto& node : canvas.Root().Children())
                        nodes.push_back(node->Id());
                    for (const auto id : nodes)
                        static_cast<void>(canvas.Root().RemoveChild(id));
                }
            }
            if (test_.directText)
            {
                TextDrawCommand text;
                text.positionPixels = {16, 16};
                text.layoutSizePixels = {96, 96};
                text.verticalAlignment = TextVerticalAlignment::Center;
                text.style.font = directFont_;
                text.style.fontSizePixels = 64;
                context.textRendering->Submit(L"A", text);
            }
            capture_ = context.ReadImage();
        }
        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override {}

    private:
        void RenderWorld(const RenderContext& context)
        {
            Float4x4 viewProjection, meshWorld, planeWorld;
            viewProjection = camera_.ViewProjectionMatrix();
            meshWorld = TranslationMatrix(0, 0, test_.planeInFront ? 2.0F : 1.0F);
            planeWorld = TranslationMatrix(0, 0, test_.planeInFront ? 1.0F : 2.0F);
            context.meshRendering->Submit(mesh_, material_, meshWorld, viewProjection, {0, 0, 1, 1},
                                          {1, 1}, {}, NoTextureIndex);
            if (test_.screenOnWorld)
                context.visual2DRendering->SubmitScreen(canvases_[0], context, {}, 0);
            else
                context.visual2DRendering->SubmitPlane(canvases_[0], context, planeWorld, {2, 2}, viewProjection);
        }
        const PixelCase& test_;
        const std::filesystem::path& imagePath_;
        std::shared_future<RgbaImage>& capture_;
        std::array<Visual2DCanvas, 2> canvases_{Visual2DCanvas{{Extent, Extent}}, Visual2DCanvas{{Extent, Extent}}};
        GpuMeshHandle mesh_;
        MaterialInstanceHandle material_;
        Camera camera_;
        FontHandle directFont_;
    };

    struct PlaneCapture
    {
        std::shared_future<RgbaImage> image;
        float logicalWidth{};
    };
    class PlaneSnapshotClient final : public IGameClient
    {
    public:
        PlaneSnapshotClient(const std::filesystem::path& imagePath, std::vector<PlaneCapture>& captures)
            : imagePath_(imagePath), captures_(captures) {}
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = 256;
            config.windowHeight = 128;
            config.autoExitAfterRenderedFrames = 8;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices& services) override
        {
            canvas_ = std::make_unique<Visual2DCanvas>(Size{64, 64}, CanvasScaleMode::Fixed);
            auto& root = canvas_->Root();
            CreateSprite(root, {-32, -32, 32, 64}).GetComponent<SpriteVisualComponent>()->SetTint(Red);
            auto& overlay = CreateSprite(root, {-24, -24, 48, 48});
            overlay_ = overlay.GetComponent<SpriteVisualComponent>();
            // Internal child Z must not create depth layers outside the flat panel.
            const auto position = overlay.Transform().Position();
            overlay.Transform().SetPosition(position.x, position.y, 100000.0F);
            CreateSprite(root, {8, 8, 12, 12}, services.visual2DRendering.LoadImageFile(imagePath_))
                .GetComponent<SpriteVisualComponent>()->SetTint(Green);
            auto& label = CreateLabel(root, {-10, -10, 20, 20}, L"M");
            auto& text = *label.GetComponent<TextVisualComponent>();
            text.SetFont({TextFontSource::System, L"Segoe UI"});
            text.SetFontSize(18);
            text.SetTextColor(White);
            mesh_ = services.meshRendering.CreateMesh<VertexPositionColor>(RectangleShape{1, 1});
            material_ = services.meshRendering.CreateMaterial(BuiltInMaterial::UnlitVertexColor);
            camera_.SetOrthographic(4, 2, .1F, 10);
        }
        bool Update(const UpdateContext&) override { return true; }
        void Render(const RenderContext& context) override
        {
            if (frame_ >= 4)
            {
                if (frame_ == 7)
                {
                    // One image page remains cached. All fifteen other blocks
                    // must be reusable after a full ring with no Canvas pass.
                    std::vector<RenderTargetTextureHandle> targets;
                    for (int index = 0; index < 15; ++index)
                        targets.push_back(context.visual2DRendering->CreateCanvasRenderTarget(1, 1));
                }
                ++frame_;
                return;
            }
            Float4x4 view;
            view = camera_.ViewProjectionMatrix();
            const auto submitMesh = [&](float width, float height, float x, float z, Float4 color)
            {
                Float4x4 world;
                world = Multiply(ScalingMatrix(width, height, 1), TranslationMatrix(x, 0, z));
                context.meshRendering->Submit(mesh_, material_, world, view, color, {1, 1}, {}, NoTextureIndex);
            };
            submitMesh(4, 2, 0, 3, {0, .25F, 0, 1});
            submitMesh(.125F, 2, -1.75F, 1, {0, 1, 1, 1});
            if (frame_ == 3) canvas_->SetFixedLogicalSize({80, 64});
            Float4x4 left, right;
            left = TranslationMatrix(-1, 0, 2);
            right = TranslationMatrix(1, 0, 2);
            overlay_->SetTint({0, 0, 1, .5F});
            context.visual2DRendering->SubmitPlane(*canvas_, context, left, {2, 2}, view);
            overlay_->SetTint({1, 1, 0, .5F});
            context.visual2DRendering->SubmitPlane(*canvas_, context, right, {2, 2}, view);
            captures_.push_back({context.ReadImage(), canvas_->LogicalSize().width});
            if (++frame_ == 4) { canvas_.reset(); overlay_ = nullptr; }
        }
        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override { canvas_.reset(); overlay_ = nullptr; }
    private:
        const std::filesystem::path& imagePath_;
        std::vector<PlaneCapture>& captures_;
        std::unique_ptr<Visual2DCanvas> canvas_;
        SpriteVisualComponent* overlay_{};
        GpuMeshHandle mesh_;
        MaterialInstanceHandle material_;
        Camera camera_;
        unsigned frame_{};
    };

    RgbaImage Capture(const PixelCase& test, const std::filesystem::path& imagePath)
    {
        std::shared_future<RgbaImage> capture;
        // Each engine run owns a fresh Win32 message queue.
        std::packaged_task<int()> run([&] {
            GraphicsDiagnostics diagnostics;
            const auto result = RunWithGraphicsDiagnostics(
                std::make_unique<PixelClient>(test, imagePath, capture), diagnostics);
            for (const auto& message : diagnostics.messages)
                std::cerr << test.name << ": " << message.description << '\n';
            return result || diagnostics.status == GraphicsDiagnostics::Status::ReadFailed ||
                !diagnostics.messages.empty() ? 1 : 0;
        });
        auto exitCode = run.get_future();
        std::jthread(std::move(run)).join();
        if (exitCode.get() != 0)
            throw std::runtime_error(std::string("Pixel fixture failed: ") + test.name);
        const auto& image = capture.get();
        if (image.width != Extent || image.height != Extent)
            throw std::runtime_error("Unexpected pixel fixture viewport size.");
        return image;
    }
    std::array<int, 3> Sample(const RgbaImage& image, std::size_t pixel)
    {
        return {image.pixels.at(pixel * 4), image.pixels.at(pixel * 4 + 1), image.pixels.at(pixel * 4 + 2)};
    }
    void TestWorldPlaneSnapshots(const std::filesystem::path& imagePath)
    {
        std::vector<PlaneCapture> captures;
        GraphicsDiagnostics diagnostics;
        std::packaged_task<int()> run([&]
        {
            return RunWithGraphicsDiagnostics(std::make_unique<PlaneSnapshotClient>(imagePath, captures), diagnostics);
        });
        auto result = run.get_future();
        std::jthread(std::move(run)).join();
        for (const auto& message : diagnostics.messages) std::cerr << message.description << '\n';
        if (result.get() != 0 || captures.size() != 4 || !diagnostics.messages.empty() ||
            diagnostics.status == GraphicsDiagnostics::Status::ReadFailed)
            throw std::runtime_error("World panel snapshot execution or GPU diagnostics failed.");
        std::cout << "World panel diagnostics: " <<
            (diagnostics.status == GraphicsDiagnostics::Status::Collected ? "collected, no errors" : "unavailable") << '\n';
        for (const auto& capture : captures)
        {
            const auto& image = capture.image.get();
            if (image.width != 256 || image.height != 128)
                throw std::runtime_error("Unexpected world panel viewport.");
            const auto requirePixel = [&](std::uint32_t x, std::uint32_t y,
                                          std::array<int, 3> expected, const char* label)
            {
                const auto actual = Sample(image, static_cast<std::size_t>(y) * image.width + x);
                for (std::size_t channel = 0; channel < 3; ++channel)
                    if (std::abs(actual[channel] - expected[channel]) > 3)
                        throw std::runtime_error(std::string(label) + " RGB=" + std::to_string(actual[0]) + ',' +
                                                 std::to_string(actual[1]) + ',' + std::to_string(actual[2]));
            };
            const auto checkLocal = [&](unsigned panel, float x, float y,
                                        std::array<int, 3> expected, const char* label)
            {
                requirePixel(panel * 128 + static_cast<std::uint32_t>((x / capture.logicalWidth + .5F) * 128),
                    static_cast<std::uint32_t>((.5F - y / 64) * 128), expected, label);
            };
            checkLocal(0, -16, -16, {128, 0, 128}, "First snapshot preserves blue over red");
            checkLocal(1, -16, -16, {255, 128, 0}, "Second snapshot uses yellow over red");
            checkLocal(0, 16, -16, {0, 32, 128}, "Blue panel alpha is applied once");
            checkLocal(1, 16, -16, {128, 160, 0}, "Yellow panel alpha is applied once");
            for (unsigned panel = 0; panel < 2; ++panel)
            {
                checkLocal(panel, 14, 14, {0, 255, 0}, "Image follows rectangle tree order");
                checkLocal(panel, 28, -28, {0, 64, 0}, "Transparent panel area reveals world geometry");
                std::size_t whitePixels{}, visibleLightPixels{};
                int peakMinimumChannel{};
                for (std::uint32_t y = 0; y < image.height; ++y)
                    for (std::uint32_t x = panel * 128; x < (panel + 1) * 128; ++x)
                    {
                        const auto rgb = Sample(image, static_cast<std::size_t>(y) * image.width + x);
                        whitePixels += rgb[0] >= 250 && rgb[1] >= 250 && rgb[2] >= 250;
                        const auto minimum = std::min({rgb[0], rgb[1], rgb[2]});
                        peakMinimumChannel = std::max(peakMinimumChannel, minimum);
                        visibleLightPixels += minimum >= 180;
                    }
                // Small antialiased glyphs are bilinearly scaled with the panel.
                // Other fixture colours have a minimum channel below 180.
                if (visibleLightPixels < 8)
                {
                    const auto directory = std::filesystem::path{"build/TestArtifacts/Visual2DPixels"};
                    std::filesystem::create_directories(directory);
                    const auto file = directory / ("world-panel-text-" + std::to_string(panel) + ".ppm");
                    std::ofstream output(file, std::ios::binary);
                    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
                    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(image.width) * image.height; ++pixel)
                        output.write(reinterpret_cast<const char*>(image.pixels.data() + pixel * 4), 3);
                    throw std::runtime_error("World panel text: white=" + std::to_string(whitePixels) +
                        ", light=" + std::to_string(visibleLightPixels) + ", peak minimum=" +
                        std::to_string(peakMinimumChannel) + ", capture=" + file.string());
                }
            }
            requirePixel(16, 8, {0, 255, 255}, "Foreground mesh occludes the panel plane");
        }
        std::cout << "World panel snapshots, mixed primitives, flat depth, resize, frame reuse, Canvas release and idle target retirement passed.\n";
    }
    std::size_t FindGlyphInterior(const RgbaImage& image)
    {
        for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(image.width) * image.height; ++pixel)
        {
            const auto color = Sample(image, pixel);
            if (color[0] >= 254 && color[1] >= 254 && color[2] >= 254)
                return pixel;
        }
        throw std::runtime_error("No fully covered reference glyph pixel was rendered.");
    }
}

int TestVisual2DPixels()
{
    try
    {
        WhiteImageFile whiteImage;
        TestWorldPlaneSnapshots(whiteImage.path);
        const PixelCase imageReference{"image-reference", {Image(White)}, {255, 255, 255}};
        if (Sample(Capture(imageReference, whiteImage.path), (Extent / 2) * Extent + Extent / 2) != imageReference.expected)
            throw std::runtime_error("The pixel fixture requires a white source image.");
        const PixelCase reference{"text-reference", {Rectangle(Blue), Text()}, {255, 255, 255}};
        const auto referenceImage = Capture(reference, whiteImage.path);
        const auto glyphPixel = FindGlyphInterior(referenceImage);
        const PixelCase directReference{
            .name = "direct-text-reference",
            .layers = {Rectangle(Blue)},
            .expected = {255, 255, 255},
            .directText = true};
        const auto directGlyphPixel = FindGlyphInterior(Capture(directReference, whiteImage.path));
        const std::vector<PixelCase> cases{
            {"rectangle-alpha-0", {Rectangle(Blue), Rectangle({1, 0, 0, 0})}, {0, 0, 255}},
            {"rectangle-alpha-half", {Rectangle(Blue), Rectangle({1, 0, 0, .5F})}, {128, 0, 128}},
            {"rectangle-alpha-1", {Rectangle(Blue), Rectangle(Red)}, {255, 0, 0}},
            {"two-translucent-rectangles", {Rectangle(Blue), Rectangle({1, 0, 0, .5F}), Rectangle({0, 1, 0, .5F})}, {64, 128, 64}},
            {"text-under-opaque-panel", {Rectangle(Blue), Text(), Rectangle(Red)}, {255, 0, 0}, false, true},
            {"text-under-translucent-panel", {Rectangle(Blue), Text(), Rectangle({1, 0, 0, .5F})}, {255, 128, 128}, false, true},
            {"text-under-opaque-image", {Rectangle(Blue), Text(), Image(Red)}, {255, 0, 0}, false, true},
            {"text-under-translucent-image", {Rectangle(Blue), Text(), Image({1, 0, 0, .5F})}, {255, 128, 128}, false, true},
            {"image-rectangle-image", {Image(Blue), Rectangle(Red), Image({0, 1, 0, .5F})}, {128, 128, 0}},
            {"rectangle-image-rectangle", {Rectangle(Blue), Image({1, 0, 0, .5F}), Rectangle(Green)}, {0, 255, 0}},
            {"canvas-z-opaque-reversed-submit", {Rectangle(Red), Rectangle(Green, 1)}, {0, 255, 0}, false, false, true},
            {"canvas-z-text-under-panel", {Rectangle(Blue), Text(), Rectangle(Red, 1)}, {255, 0, 0}, false, true, true},
            {"canvas-z-text-under-image", {Rectangle(Blue), Text(), Image(Red, 1)}, {255, 0, 0}, false, true, true},
            {"world-mesh-in-front", {Rectangle(Red)}, {0, 0, 255}, false, false, false, true, false},
            {"world-plane-in-front", {Rectangle(Red)}, {255, 0, 0}, false, false, false, true, true},
            {"world-plane-alpha-half", {Rectangle({1, 0, 0, .5F})}, {128, 0, 128}, false, false, false, true, true},
            {"world-plane-alpha-zero", {Rectangle({1, 0, 0, 0})}, {0, 0, 255}, false, false, false, true, true},
            {"world-plane-text-under-translucent-panel", {Rectangle(Blue), Text(), Rectangle({1, 0, 0, .5F})}, {255, 128, 128}, false, true, false, true, true},
            {"world-plane-text-under-image", {Rectangle(Blue), Text(), Image(Red)}, {255, 0, 0}, false, true, false, true, true},
            {"world-plane-text-above-image", {Rectangle(Blue), Image(Red), Text()}, {255, 255, 255}, false, true, false, true, true},
            {
                .name = "canvas-equal-z-submission-order",
                .layers = {Rectangle(Red), Rectangle(Green, 1)},
                .expected = {255, 0, 0},
                .reverseCanvasSubmission = true,
                .sameCanvasZ = true},
            {
                .name = "canvas-high-z-reversed-submit",
                .layers = {Rectangle(Red), Rectangle(Green, 1)},
                .expected = {0, 255, 0},
                .reverseCanvasSubmission = true,
                .highCanvasZ = true},
            {
                .name = "canvas-modal-dims-text",
                .layers = {Rectangle(Blue), Text(), Rectangle({0, 0, 0, .3F}, 1)},
                .expected = {179, 179, 179},
                .textSample = true,
                .reverseCanvasSubmission = true},
            {
                .name = "text-image-after-frame-ring",
                .layers = {Rectangle(Blue), Text(), Image(Red)},
                .expected = {255, 0, 0},
                .textSample = true,
                .frames = 6},
            {
                .name = "text-image-text",
                .layers = {Rectangle(Blue), Text(), Image(Red), Text()},
                .expected = {255, 255, 255},
                .textSample = true,
                .frames = 6},
            {
                .name = "direct-text-after-canvas",
                .layers = {Rectangle(Blue), Text(), Image(Red)},
                .expected = {255, 255, 255},
                .textSample = true,
                .directText = true,
                .frames = 6},
            {
                .name = "canvas-snapshot-outlives-nodes",
                .layers = {Rectangle(Blue), Text(), Rectangle({1, 0, 0, .5F})},
                .expected = {255, 128, 128},
                .textSample = true,
                .discardCanvas = true},
            {
                .name = "world-mesh-before-screen",
                .layers = {Rectangle(Red)},
                .expected = {0, 0, 255},
                .world = true,
                .screenOnWorld = true},
            {
                .name = "screen-before-world-mesh",
                .layers = {Rectangle(Red)},
                .expected = {255, 0, 0},
                .world = true,
                .planeInFront = true,
                .screenOnWorld = true}
        };
        unsigned guardsFailed = 0, knownFailures = 0;
        std::cout << "Case | expected RGB | actual RGB | result\n";
        for (const auto& test : cases)
        {
            const auto image = Capture(test, whiteImage.path);
            const auto actual = Sample(image, test.textSample ? (test.directText ? directGlyphPixel : glyphPixel) : (Extent / 2) * Extent + Extent / 2);
            bool matches = true;
            for (std::size_t channel = 0; channel < actual.size(); ++channel)
                matches &= std::abs(actual[channel] - test.expected[channel]) <= 3;
            if (!matches)
            {
                if (test.knownFailure) ++knownFailures;
                else ++guardsFailed;
            }
            std::cout << test.name << " | " << test.expected[0] << ',' << test.expected[1] << ',' << test.expected[2]
                      << " | " << actual[0] << ',' << actual[1] << ',' << actual[2] << " | "
                      << (matches ? "PASS" : test.knownFailure ? "KNOWN FAILURE" : "REGRESSION") << '\n';
        }
        std::cout << "Known failures=" << knownFailures << ", regression guards failed=" << guardsFailed << '\n';
        return guardsFailed ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Visual2D pixel fixture failed: " << error.what() << '\n';
        return 1;
    }
}
