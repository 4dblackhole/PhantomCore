#include "Visual2DPixelTests.h"
#include "PhantomCore.h"

#include <array>
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
            const auto image = services.visual2DRendering.LoadImage(imagePath_);
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
            DirectX::XMFLOAT4X4 viewProjection, meshWorld, planeWorld;
            DirectX::XMStoreFloat4x4(&viewProjection, camera_.ViewProjectionMatrix());
            DirectX::XMStoreFloat4x4(&meshWorld, DirectX::XMMatrixTranslation(0, 0, test_.planeInFront ? 2.0F : 1.0F));
            DirectX::XMStoreFloat4x4(&planeWorld, DirectX::XMMatrixTranslation(0, 0, test_.planeInFront ? 1.0F : 2.0F));
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
            {"world-plane-alpha-half", {Rectangle({1, 0, 0, .5F})}, {128, 0, 128}, true, false, false, true, true},
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
