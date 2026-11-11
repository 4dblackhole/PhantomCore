#pragma once

#include "Texture/TextureManager.h"
#include <d3d12.h>

// Native operations used by the engine's rendering implementation.
struct TextureRenderingBackend final
{
    static void Initialize(TextureManager& textures, ID3D12Device& device,
                           ID3D12CommandQueue& commandQueue);
    static void Shutdown(TextureManager& textures) noexcept;
    static void CollectCompletedUploads(TextureManager& textures);
    [[nodiscard]] static ID3D12DescriptorHeap* DescriptorHeap(
        const TextureManager& textures) noexcept;
    [[nodiscard]] static D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptorStart(
        const TextureManager& textures, const TextureSetHandle& textureSet);
    static void BeginRenderTargetPass(TextureManager& textures,
        ID3D12GraphicsCommandList& commands, const RenderTargetTextureHandle& target,
        const DirectX::XMFLOAT4& clearColor);
    static void EndRenderTargetPass(TextureManager& textures,
        ID3D12GraphicsCommandList& commands, const RenderTargetTextureHandle& target);
};
