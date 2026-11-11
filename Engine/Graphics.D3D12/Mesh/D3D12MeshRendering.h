#pragma once

#include "Mesh/MeshRendering.h"
#include <d3d12.h>

// Device and frame recording operations belong to the engine backend.
struct MeshRenderingBackend final
{
    static void Initialize(MeshRenderSystem& meshes, ID3D12Device& device,
        ID3D12CommandQueue& queue, DXGI_FORMAT color, DXGI_FORMAT depth);
    static void Shutdown(MeshRenderSystem& meshes) noexcept;
    [[nodiscard]] static ID3D12Device* Device(const MeshRenderSystem& meshes) noexcept;
    static void BeginFrame(MeshRenderSystem& meshes, std::uint32_t frameIndex);
    static void Flush(MeshRenderSystem& meshes, ID3D12GraphicsCommandList& commands);
};
