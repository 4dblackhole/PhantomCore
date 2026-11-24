#include "FileImageTests.h"
#include "PhantomCore.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
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
    constexpr std::uint32_t Extent = 64;
    // More distinct file images than the texture descriptor heap holds at once (16 blocks of 64).
    constexpr std::size_t PathCount = 1'200;
    constexpr std::size_t LoadsPerFrame = 40;
    constexpr unsigned LastLoadFrame = PathCount / LoadsPerFrame;
    // Frames well past the frame ring (FrameCount + 1 render indices) between the steps that wait for it.
    constexpr unsigned RingMargin = 5;
    constexpr unsigned ReleaseFrame = LastLoadFrame + 1;
    constexpr unsigned FillFrame = ReleaseFrame + RingMargin;
    constexpr unsigned ReuseFrame = FillFrame + RingMargin;
    constexpr unsigned CaptureFrame = ReuseFrame + RingMargin;
    constexpr unsigned Frames = CaptureFrame + 2;

    // A 24-bit bottom-up BMP of one color.
    void WriteBitmap(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height,
                     std::array<std::uint8_t, 3> rgb)
    {
        const std::uint32_t stride = (width * 3 + 3) & ~3U;
        const std::uint32_t size = 54 + stride * height;
        std::vector<std::uint8_t> bytes(size, 0);
        const auto put32 = [&bytes](std::size_t at, std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
        };
        bytes[0] = 'B';
        bytes[1] = 'M';
        put32(2, size);
        put32(10, 54);
        put32(14, 40);
        put32(18, width);
        put32(22, height);
        bytes[26] = 1;
        bytes[28] = 24;
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::size_t at = 54 + static_cast<std::size_t>(y) * stride + x * 3;
                bytes[at] = rgb[2];
                bytes[at + 1] = rgb[1];
                bytes[at + 2] = rgb[0];
            }
        std::ofstream file(path, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] std::uint32_t WidthOf(std::size_t index) noexcept { return 1 + static_cast<std::uint32_t>(index % 3); }

    // 1. Loads every path once, a batch per frame, releasing each batch two frames later: more images pass through
    //    than the descriptor heap holds, so this only succeeds when released file slots are reused.
    // 2. Releases everything, then fills the heap until a load is refused, releases one image and loads again at
    //    once: refused, because its slot waits for the frame ring. Some frames later the same load succeeds.
    // 3. A sprite still naming a released image draws nothing, even after its slot holds another image.
    class FileImageClient final : public IGameClient
    {
    public:
        FileImageClient(std::vector<std::string> &failures, std::filesystem::path directory,
                        std::shared_future<RgbaImage> &captured, std::size_t &filled)
            : failures_(failures), directory_(std::move(directory)), captured_(captured), filled_(filled) {}
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
            screen_.Initialize(*rendering_, {static_cast<float>(Extent), static_cast<float>(Extent)});
            for (std::size_t i = 0; i < PathCount; ++i)
            {
                paths_.push_back(directory_ / ("image-" + std::to_string(i) + ".bmp"));
                WriteBitmap(paths_.back(), WidthOf(i), 1, i % 2 == 0 ? std::array<std::uint8_t, 3>{255, 0, 0}
                                                                        : std::array<std::uint8_t, 3>{0, 0, 255});
            }
            finalPath_ = directory_ / "final.bmp";
            WriteBitmap(finalPath_, 3, 2, {0, 255, 0});
            const auto doomedPath = directory_ / "doomed.bmp";
            WriteBitmap(doomedPath, 1, 1, {255, 255, 0});
            doomed_ = rendering_->LoadImageFile(doomedPath);
            left_ = &CreateSprite(canvas_.Root(), {-32, -32, 32, 64}, {});
            right_ = &CreateSprite(canvas_.Root(), {0, -32, 32, 64}, doomed_);
            for (auto *sprite : {left_, right_})
                sprite->GetComponent<SpriteVisualComponent>()->SetTint({1, 1, 1, 1});
        }
        bool Update(const UpdateContext &) override { return true; }
        void Render(const RenderContext &context) override
        {
            ++frame_;
            if (frame_ <= LastLoadFrame)
                LoadAndRelease();
            if (frame_ == 10)
                CheckReloadAndBadFile();
            if (frame_ == ReleaseFrame)
                ReleaseAll();
            if (frame_ == FillFrame)
                FillAndRefuseRetiredSlot();
            if (frame_ == ReuseFrame)
                ReuseAfterRing();
            canvas_.SetViewportSize({static_cast<float>(context.width), static_cast<float>(context.height)});
            context.visual2DRendering->SubmitScreen(canvas_, context, {}, 0);
            if (frame_ == CaptureFrame)
                captured_ = context.ReadImage();
        }
        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override { screen_.Shutdown(); }

    private:
        ImageHandle Load(const std::filesystem::path &path)
        {
            try
            {
                return rendering_->LoadImageFile(path);
            }
            catch (const std::exception &error)
            {
                failures_.push_back("load " + path.filename().string() + " after " + std::to_string(loaded_) +
                                    " images: " + error.what());
                return {};
            }
        }

        [[nodiscard]] bool Refused(const std::filesystem::path &path)
        {
            try
            {
                live_.push_back({rendering_->LoadImageFile(path), frame_});
                return false;
            }
            catch (const std::exception &)
            {
                return true;
            }
        }

        void LoadAndRelease()
        {
            for (std::size_t n = 0; n < LoadsPerFrame && next_ < PathCount; ++n, ++next_)
            {
                const auto image = Load(paths_[next_]);
                if (!image)
                {
                    next_ = PathCount;
                    return;
                }
                ++loaded_;
                if (rendering_->LoadImageFile(paths_[next_]).value != image.value)
                    failures_.push_back("A live path must keep its handle.");
                if (rendering_->GetImageSize(image).width != WidthOf(next_))
                    failures_.push_back("Image " + std::to_string(next_) + " reports a wrong size.");
                live_.push_back({image, frame_});
            }
            // Batches two frames old are released; their slots stay unused until the frame ring passes.
            while (!live_.empty() && live_.front().frame + 2 <= frame_)
            {
                rendering_->ReleaseImage(live_.front().image);
                if (rendering_->GetImageSize(live_.front().image).width != 0)
                    failures_.push_back("A released image must report no size.");
                live_.pop_front();
            }
        }

        void CheckReloadAndBadFile()
        {
            // Paths 0 and 1 were released at frame 3, and released slots are free by now: path 0 loads again
            // as a new image in a reused slot.
            const auto again = Load(paths_[0]);
            if (!again || rendering_->GetImageSize(again).width != WidthOf(0))
                failures_.push_back("A released path must load again.");
            live_.push_back({again, frame_});
            // The screen registry forgets a released path too.
            const auto registered = screen_.RegisterImage(paths_[1]);
            screen_.ReleaseImage(registered);
            const auto reregistered = screen_.RegisterImage(paths_[1]);
            if (reregistered.value == registered.value || screen_.GetImageSize(reregistered).width != WidthOf(1))
                failures_.push_back("RegisterImage after ReleaseImage must load a new image.");
            screen_.ReleaseImage(reregistered);
            // A file that cannot be decoded is refused and leaves the free slots usable (the next loads still fit).
            if (!Refused(directory_ / "missing.bmp"))
                failures_.push_back("A missing file must be refused.");
        }

        void ReleaseAll()
        {
            for (const auto &live : live_)
                rendering_->ReleaseImage(live.image);
            live_.clear();
            // The right sprite keeps naming this one; the fill below reuses its slot.
            rendering_->ReleaseImage(doomed_);
        }

        void FillAndRefuseRetiredSlot()
        {
            std::size_t path = 0;
            while (path < PathCount && !Refused(paths_[path]))
                ++path;
            filled_ = live_.size();
            if (path == PathCount)
            {
                failures_.push_back("The descriptor heap never filled.");
                return;
            }
            // The refused path is still free to load; release one image and try again in the same frame.
            rendering_->ReleaseImage(live_.back().image);
            live_.pop_back();
            if (!Refused(paths_[path]))
                failures_.push_back("A released file slot must not be reused before the frame ring has passed.");
            refusedPath_ = path;
        }

        void ReuseAfterRing()
        {
            if (refusedPath_ >= PathCount)
                return;
            // The full heap leaves exactly the released slot: this load takes it.
            const auto image = Load(finalPath_);
            left_->GetComponent<SpriteVisualComponent>()->SetImage(image);
            if (!image || rendering_->GetImageSize(image).width != 3)
                failures_.push_back("After the frame ring a released file slot takes the next file at its own size.");
        }

        struct Live
        {
            ImageHandle image;
            unsigned frame{};
        };
        std::vector<std::string> &failures_;
        std::filesystem::path directory_;
        std::shared_future<RgbaImage> &captured_;
        std::size_t &filled_;
        Visual2DRenderSystem *rendering_{};
        ScreenVisual2DManager screen_;
        Visual2DCanvas canvas_{{Extent, Extent}};
        Visual2DNode *left_{}, *right_{};
        std::vector<std::filesystem::path> paths_;
        std::filesystem::path finalPath_;
        ImageHandle doomed_;
        std::deque<Live> live_;
        std::size_t next_{}, loaded_{}, refusedPath_{PathCount};
        unsigned frame_{};
    };

    std::array<int, 3> Sample(const RgbaImage &image, std::uint32_t x, std::uint32_t y)
    {
        const std::size_t pixel = (static_cast<std::size_t>(y) * image.width + x) * 4;
        return {image.pixels.at(pixel), image.pixels.at(pixel + 1), image.pixels.at(pixel + 2)};
    }
} // namespace

int TestFileImages()
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("phantomcore-file-images-" + std::to_string(nonce));
    std::vector<std::string> failures;
    try
    {
        std::filesystem::create_directories(directory);
        std::shared_future<RgbaImage> captured;
        std::size_t filled = 0;
        std::packaged_task<int()> run([&] {
            GraphicsDiagnostics diagnostics;
            const int result = RunWithGraphicsDiagnostics(
                std::make_unique<FileImageClient>(failures, directory, captured, filled), diagnostics);
            for (const auto &message : diagnostics.messages)
                failures.push_back("GPU: " + message.description);
            return result != 0 || diagnostics.status == GraphicsDiagnostics::Status::ReadFailed ? 1 : 0;
        });
        auto exitCode = run.get_future();
        std::jthread(std::move(run)).join();
        if (exitCode.get() != 0)
            failures.push_back("The engine run failed.");
        const auto check = [&failures](const RgbaImage &image, std::uint32_t x, std::array<int, 3> expected,
                                       const char *name) {
            const auto actual = Sample(image, x, Extent / 2);
            for (std::size_t channel = 0; channel < 3; ++channel)
                if (std::abs(actual[channel] - expected[channel]) > 3)
                {
                    failures.push_back(std::string(name) + ": " + std::to_string(actual[0]) + ',' +
                                       std::to_string(actual[1]) + ',' + std::to_string(actual[2]));
                    return;
                }
        };
        if (captured.valid())
        {
            check(captured.get(), Extent / 4, {0, 255, 0}, "file in a slot reused after the frame ring");
            check(captured.get(), Extent * 3 / 4, {0, 0, 0}, "sprite naming a released image whose slot was reused");
        }
        else
            failures.push_back("No capture was taken.");
        std::cout << "File images held at once before the heap was full: " << filled << '\n';
    }
    catch (const std::exception &error)
    {
        failures.push_back(error.what());
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    for (const auto &failure : failures)
        std::cerr << "File image test: " << failure << '\n';
    std::cout << (failures.empty() ? "File image tests passed.\n" : "File image tests failed.\n");
    return failures.empty() ? 0 : 1;
}
