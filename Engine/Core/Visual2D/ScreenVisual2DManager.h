#pragma once

#include "Visual2D/Visual2DCanvas.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>


namespace PhantomCore
{
struct RenderContext;
class Visual2DRenderSystem;



using ScreenCanvasId = std::uint64_t;
inline constexpr ScreenCanvasId InvalidScreenCanvasId = 0;

class ScreenVisual2DManager;

struct ScreenCanvasSettings
{
    Size referenceSize{1280.0F, 720.0F};
    CanvasScaleMode scaleMode{CanvasScaleMode::FixedHeight};
    Point screenOrigin{};
    std::uint32_t zOrder{};
    bool visible{};
};

// Move-only ownership token for a Canvas stored by
// ScreenVisual2DManager. Resetting or destroying the token removes the
// Canvas, so Scene cleanup cannot accidentally leave it registered.
// The token must not outlive its manager.
class ScreenCanvasHandle final
{
public:
    ScreenCanvasHandle() = default;
    ~ScreenCanvasHandle();
    ScreenCanvasHandle(const ScreenCanvasHandle&) = delete;
    ScreenCanvasHandle& operator=(const ScreenCanvasHandle&) = delete;
    ScreenCanvasHandle(ScreenCanvasHandle&& other) noexcept;
    ScreenCanvasHandle& operator=(ScreenCanvasHandle&& other) noexcept;

    [[nodiscard]] Visual2DCanvas* Get() noexcept;
    [[nodiscard]] const Visual2DCanvas* Get() const noexcept;
    [[nodiscard]] ScreenCanvasId Id() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] bool SetVisible(bool visible) noexcept;
    [[nodiscard]] bool SetPlacement(
        Point screenOrigin,
        std::uint32_t zOrder) noexcept;
    void Reset() noexcept;

private:
    friend class ScreenVisual2DManager;
    ScreenCanvasHandle(
        ScreenVisual2DManager& manager,
        ScreenCanvasId id) noexcept;

    ScreenVisual2DManager* manager_{};
    ScreenCanvasId id_{InvalidScreenCanvasId};
};

// Owns game-wide screen Canvas trees and their image registrations.
// SceneGameClient drives this service, so Scenes only mutate presentation
// state and never submit a Canvas through a render backend themselves.
class ScreenVisual2DManager final
{
public:
    ScreenVisual2DManager() = default;
    ~ScreenVisual2DManager();
    ScreenVisual2DManager(const ScreenVisual2DManager&) = delete;
    ScreenVisual2DManager& operator=(const ScreenVisual2DManager&) = delete;

    void Initialize(
        Visual2DRenderSystem& rendering,
        Size viewportSize);
    void Shutdown() noexcept;

    [[nodiscard]] ScreenCanvasId CreateCanvas(
        const ScreenCanvasSettings& settings = {});
    [[nodiscard]] ScreenCanvasHandle CreateOwnedCanvas(
        const ScreenCanvasSettings& settings = {});
    [[nodiscard]] Visual2DCanvas* FindCanvas(ScreenCanvasId id) noexcept;
    [[nodiscard]] const Visual2DCanvas* FindCanvas(
        ScreenCanvasId id) const noexcept;
    [[nodiscard]] bool RemoveCanvas(ScreenCanvasId id) noexcept;
    [[nodiscard]] bool SetCanvasVisible(
        ScreenCanvasId id,
        bool visible) noexcept;
    [[nodiscard]] bool SetCanvasPlacement(
        ScreenCanvasId id,
        Point screenOrigin,
        std::uint32_t zOrder) noexcept;

    // Paths are cached for this Client lifetime. The backend still owns
    // the GPU texture while this manager owns the game-facing registry.
    [[nodiscard]] ImageHandle RegisterImage(const std::filesystem::path& path);
    [[nodiscard]] Size GetImageSize(ImageHandle image) const noexcept;

    void Update(double elapsedSeconds);
    void Render(const RenderContext& context);
    void OnResize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] std::size_t CanvasCount() const noexcept;
    [[nodiscard]] std::size_t ImageCount() const noexcept;

private:
    struct ScreenCanvas
    {
        std::unique_ptr<Visual2DCanvas> canvas;
        Point screenOrigin{};
        std::uint32_t zOrder{};
        bool visible{};
    };

    Visual2DRenderSystem* rendering_{};
    Size viewportSize_{};
    std::map<ScreenCanvasId, ScreenCanvas> canvases_;
    std::map<std::wstring, ImageHandle, std::less<>> images_;
    ScreenCanvasId nextCanvasId_{1};
};

} // namespace PhantomCore
