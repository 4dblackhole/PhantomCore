#pragma once

// Renderer feature: D3D12 device, frame resources, and presentation.

#include "Diagnostics/EngineDiagnostics.h"
#include "Renderer/RenderFrameContext.h"
#include "Renderer/FrameResources.h"
#include "Renderer/GpuCrashDumps.h"
#include "Renderer/GpuHangTest.h"
#include "Renderer/GraphicsValidation.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>


namespace PhantomCore
{
class MeshRenderSystem;
class TextRenderSystem;
class Visual2DRenderSystem;
class FrameImageReadbackQueue;

// Internal D3D12 frame lifecycle, device, and presentation service.
// Initialize creates the persistent device/swap-chain objects.  Each
// BeginFrame/EndFrame pair records and submits one back-buffer frame;
// Frame-ring allocators and fences prevent reuse while the GPU owns them.
class D3D12Renderer final
{
public:
    D3D12Renderer();
    ~D3D12Renderer();

    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    // diagnostics decides what must be known before the device exists: DRED
    // breadcrumbs, validation, the adapter and the swap-chain shape. Its sink
    // receives validation messages while the renderer lives.
    void Initialize(
        HWND window,
        std::uint32_t width,
        std::uint32_t height,
        const EngineDiagnosticsConfig& diagnostics = {});
    void Resize(std::uint32_t width, std::uint32_t height);

    // Opens a command list, transitions/clears the current back buffer,
    // and returns the recording context for Client rendering.
    [[nodiscard]] RenderContext BeginFrame(const ClearColor& clearColor);
    // Closes/submits that list, presents without VSync, and marks the
    // current frame resource with the fence value that protects it.
    void EndFrame();
    void WaitForGpu();
    // Draw calls recorded by the most recently completed BeginFrame/EndFrame.
    [[nodiscard]] std::uint32_t LastFrameDrawCalls() const noexcept { return lastFrameDrawCalls_; }

    // Diagnostics. StartupRecord describes the adapter, its driver and the
    // swap chain, one fact per line. DeviceLossReport is empty while the
    // device works; operation and resultCode name the call that failed, when
    // one is known. With validation on it first sends the validation messages
    // still queued, and with GPU crash dumps on it waits for the driver's dump.
    // RemoveDeviceForDiagnosticsTest loses the device on purpose;
    // HangDeviceForDiagnosticsTest makes the next frame one the GPU never
    // finishes, so that Windows resets the driver.
    [[nodiscard]] std::vector<std::string> StartupRecord() const;
    [[nodiscard]] GraphicsMemoryInfo MemoryInfo() const noexcept;
    [[nodiscard]] std::optional<GraphicsDeviceLossReport> DeviceLossReport(
        const char* operation,
        std::uint32_t resultCode) const;
    void RemoveDeviceForDiagnosticsTest();
    void HangDeviceForDiagnosticsTest() noexcept;

    [[nodiscard]] ID3D12Device* Device() const noexcept;
    [[nodiscard]] MeshRenderSystem& MeshRendering() const noexcept;
    [[nodiscard]] TextRenderSystem& TextRendering() const noexcept;
    [[nodiscard]] Visual2DRenderSystem& Visual2DRendering() const noexcept;
    [[nodiscard]] DXGI_FORMAT RenderTargetFormat() const noexcept;
    [[nodiscard]] DXGI_FORMAT DepthStencilFormat() const noexcept;
    [[nodiscard]] bool SupportsTearing() const noexcept;

private:
    // The swap chain may hold more back buffers than the frame ring has slots.
    // Rendering into a back buffer is safe once the frame that used it last
    // has completed, and BeginFrame waits for the frame FrameCount back.
    static constexpr std::uint32_t MaximumSwapChainBuffers = 3;

    void CreateDeviceAndFactory();
    void EnableLossDiagnostics();
    void RecordHangIfRequested();
    void CreateCommandObjects();
    void CreateSwapChain(HWND window);
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateDepthStencil();
    void UpdateViewport() noexcept;
    void WaitForFrame(std::uint32_t frameIndex);
    void WaitForSwapChain() const noexcept;
    [[nodiscard]] UINT SwapChainFlags() const noexcept;
    [[nodiscard]] std::string DescribeSwapChain() const;
    // Validation: everything but the device goes first, so the device's
    // live-object report shows only what something else still holds.
    void ReleaseAllButDevice() noexcept;

    HWND window_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    // Slot of the frame ring (allocators, arenas, fences) and the swap chain's
    // current back buffer. They advance together but are separate numbers.
    std::uint32_t frameIndex_{};
    std::uint32_t backBufferIndex_{};
    std::uint64_t nextRenderIndex_{1};
    std::uint32_t rtvDescriptorSize_{};
    bool tearingSupported_{};
    bool deviceRemovedData_{};
    bool deviceRemovedDataRequested_{};
    bool gpuCrashDumpsRequested_{};
    bool hangRequested_{};
    bool frameOpen_{};
    std::uint32_t lastFrameDrawCalls_{};
    GraphicsDeviceDiagnosticsConfig graphics_;
    std::uint32_t swapChainBuffers_{D3D12Detail::FrameCount};
    D3D12Detail::ValidationState validation_;
    D3D12Detail::GpuCrashDumps gpuCrashDumps_;
    D3D12Detail::GpuHangObjects gpuHang_;
    // "default", "WARP", "index 1 (not found, default used)", ...
    std::string adapterRequest_;
    IEngineDiagnosticSink* sink_{};
    // Mutable: DeviceLossReport sends what the queue still holds before the
    // report, since the failed frame never reached EndFrame's forwarding.
    mutable D3D12Detail::ValidationMessageRelay validationMessages_;
    HANDLE frameLatencyWait_{};

    DXGI_FORMAT renderTargetFormat_{DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_FORMAT depthStencilFormat_{DXGI_FORMAT_D32_FLOAT};
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissorRectangle_{};
    RenderFrameContext frameContext_;

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    // Kept for the diagnostics: its description and the memory queries.
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, MaximumSwapChainBuffers>
        renderTargets_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, D3D12Detail::FrameCount>
        commandAllocators_{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::array<std::uint64_t, D3D12Detail::FrameCount> frameFenceValues_{};
    std::uint64_t nextFenceValue_{1};
    HANDLE fenceEvent_{};
    std::unique_ptr<MeshRenderSystem> meshRenderSystem_;
    std::unique_ptr<TextRenderSystem> textRenderSystem_;
    // Declared after its dependencies so it is destroyed before them.
    std::unique_ptr<Visual2DRenderSystem> visual2DRenderSystem_;
    std::unique_ptr<FrameImageReadbackQueue> imageReadbacks_;
};

} // namespace PhantomCore
