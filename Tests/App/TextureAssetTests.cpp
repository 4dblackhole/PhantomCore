#include "TextureAssetTests.h"
#include "PhantomCore.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace PhantomCore;

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    std::vector<std::filesystem::path> ImagePaths(const std::filesystem::path& root)
    {
        std::vector<std::filesystem::path> paths;
        constexpr std::array extensions{L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff", L".ico"};
        for (const auto& file : std::filesystem::recursive_directory_iterator(root))
        {
            if (!file.is_regular_file()) continue;
            auto extension = file.path().extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t c)
                { return static_cast<wchar_t>(std::towlower(c)); });
            if (std::ranges::find(extensions, extension) != extensions.end()) paths.push_back(file.path());
        }
        std::ranges::sort(paths);
        Require(!paths.empty(), "The requested asset root must contain WIC images.");
        return paths;
    }
    struct Report
    {
        std::size_t loaded{};
        unsigned frames{};
        bool shutdown{};
    };
    class AssetClient final : public IGameClient
    {
    public:
        AssetClient(std::vector<std::filesystem::path> paths, Report& report)
            : paths_(std::move(paths)), report_(report) {}
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.windowWidth = 768;
            config.windowHeight = 768;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices& services) override
        {
            const auto columns = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(paths_.size()))));
            const auto rows = (paths_.size() + columns - 1) / columns;
            constexpr float cell = 48;
            const Size size{static_cast<float>(columns) * cell, static_cast<float>(rows) * cell};
            canvas_ = std::make_unique<Visual2DCanvas>(size, CanvasScaleMode::Fixed);
            for (std::size_t index = 0; index < paths_.size(); ++index)
            {
                const auto image = services.visual2DRendering.LoadImage(paths_[index]);
                const auto extent = services.visual2DRendering.GetImageSize(image);
                Require(image.value != 0 && extent.width > 0 && extent.height > 0,
                    "Every requested asset must have a valid image handle and decoded extent.");
                Require(services.visual2DRendering.LoadImage(paths_[index]).value == image.value,
                    "Repeated image paths must reuse their registered handle.");
                auto& node = CreateSprite(canvas_->Root(),
                    {-size.width * .5F + static_cast<float>(index % columns) * cell,
                      size.height * .5F - static_cast<float>(index / columns + 1) * cell,
                      cell, cell});
                node.GetComponent<SpriteVisualComponent>()->SetImage(image);
                ++report_.loaded;
            }
        }
        bool Update(const UpdateContext& context) override
        {
            Require(context.totalSeconds < 30, "Asset texture rendering timed out.");
            return report_.frames < 4;
        }
        void Render(const RenderContext& context) override
        {
            context.visual2DRendering->SubmitScreen(*canvas_, context);
            ++report_.frames;
        }
        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override { canvas_.reset(); report_.shutdown = true; }
    private:
        std::vector<std::filesystem::path> paths_;
        Report& report_;
        std::unique_ptr<Visual2DCanvas> canvas_;
    };
}

int TestTextureAssets(const char* root)
{
    try
    {
        auto paths = ImagePaths(std::filesystem::path{root});
        const auto expected = paths.size();
        Report report;
        GraphicsDiagnostics diagnostics;
        int result = 1;
        std::jthread engine([&]
        {
            result = RunWithGraphicsDiagnostics(std::make_unique<AssetClient>(std::move(paths), report), diagnostics);
        });
        engine.join();
        Require(result == 0 && report.shutdown && report.loaded == expected && report.frames == 4,
            "Asset registration and actual rendering must finish before engine shutdown.");
        Require(diagnostics.status != GraphicsDiagnostics::Status::ReadFailed,
            "Graphics diagnostics could not be read.");
        for (const auto& message : diagnostics.messages) std::cerr << message.description << '\n';
        Require(diagnostics.messages.empty(), "Asset rendering produced GPU errors or corruption.");
        std::cout << "WIC assets loaded and rendered: " << report.loaded << "; frames: " << report.frames
            << "; diagnostics: " << (diagnostics.status == GraphicsDiagnostics::Status::Collected
                ? "collected, no errors" : "unavailable") << '\n';
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
