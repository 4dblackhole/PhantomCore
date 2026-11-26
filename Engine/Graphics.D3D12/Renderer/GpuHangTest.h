#pragma once

// Renderer feature: a draw the GPU never finishes, for the diagnostics test
// that wants a real GPU timeout. Windows resets the driver after about two
// seconds; the screen goes dark for a moment and other programs using the
// GPU may lose their devices too. Nothing calls this in a normal run.

#include <Windows.h>
#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

namespace PhantomCore
{
namespace D3D12Detail
{
    // Must stay alive until the device is released: the queued draw uses them.
    struct GpuHangObjects
    {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    };

    // Records one full-screen draw whose pixel shader loops forever into the
    // render target and depth buffer already bound on commandList.
    [[nodiscard]] GpuHangObjects RecordGpuHang(
        ID3D12Device& device,
        ID3D12GraphicsCommandList& commandList,
        DXGI_FORMAT renderTargetFormat,
        DXGI_FORMAT depthStencilFormat);
}

} // namespace PhantomCore
