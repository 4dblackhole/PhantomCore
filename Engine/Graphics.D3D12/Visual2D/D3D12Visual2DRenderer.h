#pragma once

// Internal D3D12 implementation of the public Visual2D rendering service.
// Client projects receive Visual2DRenderSystem and cannot access this
// backend's initialization or shutdown operations through PhantomCore.h.

#include "Visual2D/Visual2DRendering.h"

#include <d3d12.h>

#include <memory>
#include <vector>


namespace PhantomCore
{
class TextRenderSystem;

class D3D12Visual2DRenderer final : public Visual2DRenderSystem
{
public:
    D3D12Visual2DRenderer();
    ~D3D12Visual2DRenderer() override;

    D3D12Visual2DRenderer(const D3D12Visual2DRenderer&) = delete;
    D3D12Visual2DRenderer& operator=(const D3D12Visual2DRenderer&) = delete;

    void Initialize(
        MeshRenderSystem& meshRendering,
        TextRenderSystem& textRendering,
        DXGI_FORMAT depthStencilFormat);
    void Shutdown() noexcept;
    void BeginScreenFrame(std::uint64_t renderIndex, std::uint32_t frameIndex);
    void DrawScreen(ID3D12GraphicsCommandList& commands, std::uint32_t frameIndex,
                    std::uint32_t width, std::uint32_t height);

    [[nodiscard]] ImageHandle LoadImageFile(
        const std::filesystem::path& path) override;
    bool PrepareImages(std::span<const std::filesystem::path> paths, std::string &error) override;
    [[nodiscard]] Size GetImageSize(
        ImageHandle image) const noexcept override;
    void SubmitScreen(
        const Visual2DCanvas& canvas,
        const RenderContext& context,
        Point screenOrigin,
        std::uint32_t canvasZOrder) override;
    void SubmitPlane(
        const Visual2DCanvas& canvas,
        const RenderContext& context,
        const Float4x4& surfaceWorld,
        Size surfaceWorldSize,
        const Float4x4& viewProjection) override;
    [[nodiscard]] RenderTargetTextureHandle CreateCanvasRenderTarget(
        std::uint32_t width,
        std::uint32_t height) override;
    void RenderToTexture(
        const Visual2DCanvas& canvas,
        const RenderTargetTextureHandle& target,
        const RenderContext& context) override;

private:
    void RecordCanvasTexture(Size logicalSize, std::vector<DrawPacket> commands,
        const RenderTargetTextureHandle& target, const RenderContext& context,
        bool flattenDepth);
    [[nodiscard]] bool IsInitialized() const noexcept;

    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace PhantomCore
