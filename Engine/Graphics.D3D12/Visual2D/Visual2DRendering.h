#pragma once

// Client-facing Visual2D rendering service. The engine owns one concrete
// backend and exposes only Canvas/resource operations through this contract.

#include "Visual2D/Visual2DCanvas.h"
#include "Texture/TextureManager.h"
#include "Rendering/RenderContext.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>


class Visual2DRenderSystem
{
public:
    virtual ~Visual2DRenderSystem() = default;

    Visual2DRenderSystem(const Visual2DRenderSystem&) = delete;
    Visual2DRenderSystem& operator=(const Visual2DRenderSystem&) = delete;

    // Loads a PNG/WIC-supported image once and returns an opaque handle
    // that can be assigned to a SpriteVisualComponent or widget style.
    [[nodiscard]] virtual ImageHandle LoadImage(
        const std::filesystem::path& path) = 0;
    // Start/poll CPU decoding without waiting. On true, inspect error;
    // successful images can then be registered on the render thread.
    virtual bool PrepareImages(std::span<const std::filesystem::path>, std::string &error)
    {
        error.clear();
        return true;
    }

    // Returns the original decoded pixel extent for an image handle.
    // Invalid or expired handles return a zero size. Sprite bounds remain
    // a Client presentation choice; this lets them preserve source aspect
    // ratios when scaling to a game-specific target width or height.
    [[nodiscard]] virtual Size GetImageSize(
        ImageHandle image) const noexcept = 0;

    // Renders a centered, Y-up Canvas at pixel size. screenOrigin remains
    // the Canvas panel's top-left point in Win32 screen pixels. A larger
    // Canvas Z-order places the complete tree in front of a smaller one.
    // Equal Z uses submission order. The engine snapshots the draw list and
    // composites rectangles, images and text in tree paint order at frame end.
    // Depth testing against world geometry uses bands clamped at Z 31.
    // Direct TextRenderSystem submissions follow all screen Canvases.
    virtual void SubmitScreen(
        const Visual2DCanvas& canvas,
        const RenderContext& context,
        Point screenOrigin = {},
        std::uint32_t canvasZOrder = 0) = 0;

    // Renders rectangles directly onto a finite local XY plane. Use
    // RenderToTexture plus a textured mesh when text or curvature is
    // required. Input mapping remains independent of either path.
    virtual void SubmitPlane(
        const Visual2DCanvas& canvas,
        const RenderContext& context,
        const DirectX::XMFLOAT4X4& surfaceWorld,
        Size surfaceWorldSize,
        const DirectX::XMFLOAT4X4& viewProjection) = 0;

    [[nodiscard]] virtual RenderTargetTextureHandle CreateCanvasRenderTarget(
        std::uint32_t width,
        std::uint32_t height) = 0;

    // Records an immediate off-screen pass. Rectangles, images, and
    // DirectWrite glyphs are rendered into target, transitioned to an
    // SRV, and can then be sampled by a curved mesh later in the frame.
    virtual void RenderToTexture(
        const Visual2DCanvas& canvas,
        const RenderTargetTextureHandle& target,
        const RenderContext& context) = 0;

protected:
    Visual2DRenderSystem() = default;
};
