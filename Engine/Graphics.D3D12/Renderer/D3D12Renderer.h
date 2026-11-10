#pragma once

// Renderer feature: D3D12 device, frame resources, and presentation.

#include "Rendering/ImageReadback.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <future>
#include <memory>


class MeshRenderSystem;
class TextRenderSystem;
class Visual2DRenderSystem;
class FrameImageReadbackQueue;

struct ClearColor
{
    float red{};
    float green{};
    float blue{};
    float alpha{1.0F};
};

struct RenderContext
{
    ID3D12GraphicsCommandList* commandList{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissorRectangle{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t frameIndex{};
    // Monotonic BeginFrame sequence, distinct from the reusable back-
    // buffer index. Pass adapters use it to detect duplicate work within
    // one frame even after a feature was inactive for several frames.
    std::uint64_t renderIndex{};
    DXGI_FORMAT renderTargetFormat{DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_FORMAT depthStencilFormat{DXGI_FORMAT_D32_FLOAT};
    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView{};
    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView{};
    // Non-owning queue/service for high-level mesh submissions during
    // this BeginFrame/EndFrame pair.
    MeshRenderSystem* meshRendering{};
    // Non-owning queue/service for screen-space text submissions.
    TextRenderSystem* textRendering{};
    // Non-owning engine-wide Visual2D service. Scenes submit Canvas data
    // through this pointer but never initialize or release the renderer.
    Visual2DRenderSystem* visual2DRendering{};

    // Request the final RGBA image of this frame on the rendering thread.
    // Repeated requests share a result. Poll the future without blocking Update/
    // Render; completion occurs at an existing frame-resource or shutdown wait.
    // The completed CPU image remains usable after the engine is destroyed.
    [[nodiscard]] std::shared_future<RgbaImage> ReadImage() const;

private:
    friend class D3D12Renderer;
    FrameImageReadbackQueue* imageReadbacks_{};
};

// Public D3D12 frame lifecycle, device, and presentation service.
// Initialize creates the persistent device/swap-chain objects.  Each
// BeginFrame/EndFrame pair records and submits one back-buffer frame;
// FrameCount allocators and fences prevent reuse while the GPU owns them.
class D3D12Renderer final
{
public:
    static constexpr std::uint32_t FrameCount = 2;

    D3D12Renderer();
    ~D3D12Renderer();

    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    void Initialize(
        HWND window,
        std::uint32_t width,
        std::uint32_t height);
    void Resize(std::uint32_t width, std::uint32_t height);

    // Opens a command list, transitions/clears the current back buffer,
    // and returns the recording context for Client rendering.
    [[nodiscard]] RenderContext BeginFrame(const ClearColor& clearColor);
    // Closes/submits that list, presents without VSync, and marks the
    // current frame resource with the fence value that protects it.
    void EndFrame();
    void WaitForGpu();

    [[nodiscard]] ID3D12Device* Device() const noexcept;
    [[nodiscard]] MeshRenderSystem& MeshRendering() const noexcept;
    [[nodiscard]] TextRenderSystem& TextRendering() const noexcept;
    [[nodiscard]] Visual2DRenderSystem& Visual2DRendering() const noexcept;
    [[nodiscard]] DXGI_FORMAT RenderTargetFormat() const noexcept;
    [[nodiscard]] DXGI_FORMAT DepthStencilFormat() const noexcept;
    [[nodiscard]] bool SupportsTearing() const noexcept;

private:
    void CreateDeviceAndFactory();
    void CreateCommandObjects();
    void CreateSwapChain(HWND window);
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateDepthStencil();
    void UpdateViewport() noexcept;
    void WaitForFrame(std::uint32_t frameIndex);

    HWND window_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint32_t frameIndex_{};
    std::uint64_t nextRenderIndex_{1};
    std::uint32_t rtvDescriptorSize_{};
    bool tearingSupported_{};
    bool frameOpen_{};

    DXGI_FORMAT renderTargetFormat_{DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_FORMAT depthStencilFormat_{DXGI_FORMAT_D32_FLOAT};
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissorRectangle_{};

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount>
        renderTargets_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, FrameCount>
        commandAllocators_{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::array<std::uint64_t, FrameCount> frameFenceValues_{};
    std::uint64_t nextFenceValue_{1};
    HANDLE fenceEvent_{};
    std::unique_ptr<MeshRenderSystem> meshRenderSystem_;
    std::unique_ptr<TextRenderSystem> textRenderSystem_;
    // Declared after its dependencies so it is destroyed before them.
    std::unique_ptr<Visual2DRenderSystem> visual2DRenderSystem_;
    std::unique_ptr<FrameImageReadbackQueue> imageReadbacks_;
};
