#include "TextureLifetimeTests.h"
#include "PhantomCore.h"
#include <array>
#include <chrono>
#include <future>
#include <vector>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace PhantomCore;

namespace
{
    void Require(bool value, const char *message)
    {
        if (!value)
            throw std::runtime_error(message);
    }
    struct Report
    {
        unsigned frames{};
        bool shutdown{};
        TextureSetHandle retained;
        std::vector<std::shared_future<RgbaImage>> captures;
    };
    class TextureLifetimeClient final : public IGameClient
    {
      public:
        explicit TextureLifetimeClient(Report &report) : report_(report)
        {
        }
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = 128;
            config.windowHeight = 64;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices &services) override
        {
            textures_ = &services.meshRendering.Textures();
            rendering_ = &services.visual2DRendering;
            mesh_ = services.meshRendering.CreateMesh<VertexPositionUvColor>(RectangleShape{});
            camera_.SetOrthographic(4, 2, .1F, 10);
            CreateSprite(redCanvas_.Root(), {-8, -8, 16, 16})
                .GetComponent<SpriteVisualComponent>()->SetTint({1, 0, 0, 1});
            CreateSprite(blueCanvas_.Root(), {-8, -8, 16, 16})
                .GetComponent<SpriteVisualComponent>()->SetTint({0, 0, 1, 1});
            for (int i = 0; i < 200; ++i)
                static_cast<void>(textures_->CreateRenderTargetTexture(1, 1));
            std::vector<RenderTargetTextureHandle> live;
            for (int i = 0; i < 16; ++i)
                live.push_back(textures_->CreateRenderTargetTexture(1, 1));
            bool refused = false;
            try
            {
                static_cast<void>(textures_->CreateRenderTargetTexture(1, 1));
            }
            catch (const std::runtime_error &)
            {
                refused = true;
            }
            Require(refused, "Live texture sets must never share a descriptor block.");
            live.clear();
            static_cast<void>(textures_->CreateRenderTargetTexture(1, 1));
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            imagePath_ =
                std::filesystem::temp_directory_path() / ("phantomcore-texture-lifetime-" + std::to_string(nonce) + ".bmp");
            constexpr std::array<unsigned char, 58> image{0x42, 0x4d, 58, 0, 0, 0, 0, 0, 0, 0,    54,   0,    0, 0,  40,
                                                          0,    0,    0,  1, 0, 0, 0, 1, 0, 0,    0,    1,    0, 24, 0,
                                                          0,    0,    0,  0, 4, 0, 0, 0, 0, 0,    0,    0,    0, 0,  0,
                                                          0,    0,    0,  0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0};
            std::ofstream file(imagePath_, std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(reinterpret_cast<const char *>(image.data()), image.size());
            file.close();
            const std::array paths{imagePath_};
            auto upload = textures_->LoadTextureSet(paths);
            firstUpload_ = upload;
            upload.reset();
            Require(!firstUpload_.expired(), "A submitted upload must retain its descriptor lease until its fence.");
            report_.retained = textures_->LoadTextureSet(paths);
            const auto originalSize = report_.retained->Size();
            auto wrongThread = std::async(std::launch::async, [this]
            {
                try { static_cast<void>(textures_->AppendTexture(report_.retained, imagePath_)); }
                catch (const std::logic_error&) { return true; }
                return false;
            });
            Require(wrongThread.get() && report_.retained->Size() == originalSize,
                "A foreign thread must be rejected before texture mutation.");
        }
        bool Update(const UpdateContext &context) override
        {
            Require(context.totalSeconds < 15, "Texture lifetime regression timed out.");
            return report_.frames < 64;
        }
        void Render(const RenderContext &context) override
        {
            if (context.frameIndex >= targets_.size())
                throw std::runtime_error("Unexpected frame-ring size.");
            const auto previous = targets_[context.frameIndex];
            auto target = textures_->CreateRenderTargetTexture(16, 16);
            rendering_->RenderToTexture(redCanvas_, target, context);
            const auto originalInfo = target->Textures()->Info(0);
            const auto originalHandle = target->Textures();
            const auto appended = textures_->AppendTexture(originalHandle, imagePath_);
            Require(appended == originalHandle && appended->Size() == 2 &&
                appended->Info(0).width == originalInfo.width && appended->Info(0).height == originalInfo.height,
                "Appending must keep the same handle and preserve prior slots.");
            Require(previous.expired(), "A reused frame slot must release its completed target.");
            targets_[context.frameIndex] = target;
            SubmitTarget(context, target->Textures(), -1);
            target.reset();
            Require(!targets_[context.frameIndex].expired(), "Recorded GPU work must retain its target.");
            auto another = textures_->CreateRenderTargetTexture(16, 16);
            rendering_->RenderToTexture(blueCanvas_, another, context);
            SubmitTarget(context, another->Textures(), 1);
            another.reset();
            report_.captures.push_back(context.ReadImage());
            ++report_.frames;
            if (report_.frames >= 4)
                Require(firstUpload_.expired(), "Completed upload leases must become reusable.");
            const std::array paths{imagePath_};
            static_cast<void>(textures_->LoadTextureSet(paths));
        }
        void OnResize(std::uint32_t, std::uint32_t) override
        {
        }
        void Shutdown() noexcept override
        {
            std::error_code ignored;
            std::filesystem::remove(imagePath_, ignored);
            report_.shutdown = true;
        }

      private:
        void SubmitTarget(const RenderContext& context, const TextureSetHandle& textures,
                          const float x)
        {
            auto material = context.meshRendering->CreateMaterial(
                BuiltInMaterial::UnlitVertexColorTextureArrayAlphaBlend);
            material->SetTextureSet(textures);
            DirectX::XMFLOAT4X4 world, viewProjection;
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranslation(x, 0, 1));
            DirectX::XMStoreFloat4x4(&viewProjection, camera_.ViewProjectionMatrix());
            context.meshRendering->Submit(mesh_, material, world, viewProjection,
                                          {1, 1, 1, 1}, {1, 1}, {}, 0);
        }

        Report &report_;
        TextureManager *textures_{};
        Visual2DRenderSystem *rendering_{};
        Visual2DCanvas redCanvas_{{16, 16}, CanvasScaleMode::Fixed};
        Visual2DCanvas blueCanvas_{{16, 16}, CanvasScaleMode::Fixed};
        GpuMeshHandle mesh_;
        Camera camera_;
        std::filesystem::path imagePath_;
        std::weak_ptr<const TextureSet> firstUpload_;
        std::array<std::weak_ptr<RenderTargetTexture>, 16> targets_;
    };
    class NextGenerationClient final : public IGameClient
    {
    public:
        explicit NextGenerationClient(TextureSetHandle& old) : old_(old) {}
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices& services) override
        {
            auto& textures = services.meshRendering.Textures();
            bool rejected = false;
            try { static_cast<void>(textures.AppendTexture(old_, "missing-texture-generation.bmp")); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "An old-generation handle must be rejected before decoding its path.");
            std::vector<RenderTargetTextureHandle> live;
            for (int index = 0; index < 16; ++index)
                live.push_back(textures.CreateRenderTargetTexture(1, 1));
            old_.reset();
            bool refused = false;
            try { static_cast<void>(textures.CreateRenderTargetTexture(1, 1)); }
            catch (const std::runtime_error&) { refused = true; }
            Require(refused, "Releasing an old generation must not free a live slot in the new heap.");
            live.pop_back();
            static_cast<void>(textures.CreateRenderTargetTexture(1, 1));
        }
        bool Update(const UpdateContext&) override { return false; }
        void Render(const RenderContext&) override {}
        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override {}
    private:
        TextureSetHandle& old_;
    };

    int RunOnFreshThread(std::unique_ptr<IGameClient> client, GraphicsDiagnostics& diagnostics)
    {
        int result = 1;
        std::jthread engine([&] { result = RunWithGraphicsDiagnostics(std::move(client), diagnostics); });
        engine.join();
        Require(diagnostics.status != GraphicsDiagnostics::Status::ReadFailed,
                "Graphics diagnostics must be readable when available.");
        for (const auto& message : diagnostics.messages) std::cerr << message.description << '\n';
        Require(diagnostics.messages.empty(), "Texture lifetime checks must not produce GPU errors or corruption.");
        std::cout << "Graphics diagnostics: " <<
            (diagnostics.status == GraphicsDiagnostics::Status::Collected ? "collected, no errors" : "unavailable") << '\n';
        return result;
    }
    std::array<std::uint8_t, 3> Sample(const RgbaImage& image, const std::uint32_t x)
    {
        const auto offset = (static_cast<std::size_t>(image.height / 2) * image.width + x) * 4;
        return {image.pixels.at(offset), image.pixels.at(offset + 1), image.pixels.at(offset + 2)};
    }
} // namespace
int TestTextureLifetime()
{
    Report report;
    GraphicsDiagnostics firstDiagnostics;
    const auto exitCode = RunOnFreshThread(std::make_unique<TextureLifetimeClient>(report), firstDiagnostics);
    if (exitCode || report.frames != 64 || !report.shutdown || report.captures.size() != 64)
        return 1;
    for (const auto& capture : report.captures)
    {
        const auto& image = capture.get();
        const auto left = Sample(image, image.width / 4);
        const auto right = Sample(image, image.width * 3 / 4);
        Require(left == std::array<std::uint8_t, 3>{255, 0, 0} &&
                    right == std::array<std::uint8_t, 3>{0, 0, 255},
                "Red and blue targets must retain independent contents through presentation.");
    }
    GraphicsDiagnostics nextDiagnostics;
    if (RunOnFreshThread(std::make_unique<NextGenerationClient>(report.retained), nextDiagnostics) != 0) return 1;
    std::cout << "Owner-thread rejection, append slot preservation and renderer-generation isolation passed.\n";
    std::cout << "Descriptor reuse: 200 released targets, live-capacity limit "
                 "and 64 GPU frames with independent target colors passed.\n";
    return 0;
}
