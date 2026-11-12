#include "Renderer/D3D12Renderer.h"
#include "Mesh/D3D12MeshRendering.h"
#include "Text/D3D12TextRendering.h"
#include "Renderer/FrameImageReadbackQueue.h"

#include "Mesh/MeshRendering.h"
#include "Text/TextRendering.h"
#include "Visual2D/D3D12Visual2DRenderer.h"

#include <d3d12sdklayers.h>

#include <stdexcept>
#include <string>
#include <utility>


namespace PhantomCore
{
namespace
{

    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation) + " failed with HRESULT 0x" +
                [] (const HRESULT value)
                {
                    constexpr char digits[] = "0123456789ABCDEF";
                    std::string text(8, '0');
                    const auto bits = static_cast<std::uint32_t>(value);
                    for (std::size_t index = 0; index < text.size(); ++index)
                    {
                        const std::size_t shift = (text.size() - index - 1) * 4;
                        text[index] = digits[(bits >> shift) & 0xF];
                    }
                    return text;
                }(result));
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE OffsetDescriptor(
        const D3D12_CPU_DESCRIPTOR_HANDLE start,
        const std::uint32_t index,
        const std::uint32_t descriptorSize) noexcept
    {
        D3D12_CPU_DESCRIPTOR_HANDLE result = start;
        result.ptr +=
            static_cast<SIZE_T>(index) *
            static_cast<SIZE_T>(descriptorSize);
        return result;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> SelectAdapter(
        IDXGIFactory6& factory)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;
             factory.EnumAdapterByGpuPreference(
                 index,
                 DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                 IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf())) !=
             DXGI_ERROR_NOT_FOUND;
             ++index)
        {
            DXGI_ADAPTER_DESC1 description{};
            adapter->GetDesc1(&description);
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                continue;
            }

            if (SUCCEEDED(D3D12CreateDevice(
                    adapter.Get(),
                    D3D_FEATURE_LEVEL_11_0,
                    __uuidof(ID3D12Device),
                    nullptr)))
            {
                return adapter;
            }
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(
            factory.EnumWarpAdapter(
                IID_PPV_ARGS(warpAdapter.ReleaseAndGetAddressOf())),
            "IDXGIFactory6::EnumWarpAdapter");
        ThrowIfFailed(
            warpAdapter.As(&adapter),
            "Query WARP IDXGIAdapter1");
        return adapter;
    }
}

std::shared_future<RgbaImage> RenderContext::ReadImage() const
{
    const auto* frame = RenderFrameContext::Find(*this);
    if (frame == nullptr || frame->imageReadbacks == nullptr)
        throw std::logic_error("ReadImage requires an active engine RenderContext.");
    return frame->imageReadbacks->Request(renderIndex);
}

D3D12Renderer::D3D12Renderer() = default;

D3D12Renderer::~D3D12Renderer()
{
    if (device_ != nullptr && commandQueue_ != nullptr && fence_ != nullptr)
    {
        try
        {
            WaitForGpu();
        }
        catch (...)
        {
        }
    }

    if (fenceEvent_ != nullptr)
    {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

void D3D12Renderer::Initialize(
    const HWND window,
    const std::uint32_t width,
    const std::uint32_t height)
{
    window_ = window;
    width_ = width;
    height_ = height;

    // The creation order follows D3D12 dependencies: device first,
    // command queue/fences next, then a swap chain that uses that queue.
    CreateDeviceAndFactory();
    CreateCommandObjects();
    CreateSwapChain(window);
    CreateDescriptorHeaps();
    CreateRenderTargets();
    CreateDepthStencil();
    UpdateViewport();

    meshRenderSystem_ = std::make_unique<MeshRenderSystem>();
    MeshRenderingBackend::Initialize(
        *meshRenderSystem_,
        *device_.Get(),
        *commandQueue_.Get(),
        renderTargetFormat_,
        depthStencilFormat_);

    textRenderSystem_ = std::make_unique<TextRenderSystem>();
    TextRenderingBackend::Initialize(
        *textRenderSystem_,
        *device_.Get(),
        renderTargetFormat_,
        depthStencilFormat_);

    // Visual2D is one engine-owned service. Scenes share its pipelines,
    // texture pages, font atlases, and frame upload arenas.
    auto visual2DBackend =
        std::make_unique<D3D12Visual2DRenderer>();
    visual2DBackend->Initialize(
        *meshRenderSystem_,
        *textRenderSystem_,
        depthStencilFormat_);
    visual2DRenderSystem_ = std::move(visual2DBackend);
    imageReadbacks_ = std::make_unique<FrameImageReadbackQueue>(*device_.Get(), D3D12Detail::FrameCount);
}

void D3D12Renderer::Resize(
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (width == 0 || height == 0 ||
        (width == width_ && height == height_))
    {
        return;
    }

    // Resize replaces the swap-chain buffers, so no old buffer may still
    // be referenced by queued GPU commands.
    WaitForGpu();

    for (auto& target : renderTargets_)
    {
        target.Reset();
    }
    depthStencil_.Reset();

    const UINT flags = tearingSupported_
        ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        : 0;

    ThrowIfFailed(
        swapChain_->ResizeBuffers(
            D3D12Detail::FrameCount,
            width,
            height,
            renderTargetFormat_,
            flags),
        "IDXGISwapChain4::ResizeBuffers");

    width_ = width;
    height_ = height;
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    frameFenceValues_.fill(0);
    CreateRenderTargets();
    CreateDepthStencil();
    UpdateViewport();
}

RenderContext D3D12Renderer::BeginFrame(const ClearColor& clearColor)
{
    if (frameOpen_)
    {
        throw std::logic_error("A D3D12 frame is already open.");
    }

    // This is the normal per-frame synchronization point.  It waits only
    // when the allocator/back buffer selected for reuse is still in use;
    // it never waits for the entire GPU after every render.
    WaitForFrame(frameIndex_);
    imageReadbacks_->CompleteFrame(frameIndex_);
    ThrowIfFailed(
        commandAllocators_[frameIndex_]->Reset(),
        "ID3D12CommandAllocator::Reset");
    ThrowIfFailed(
        commandList_->Reset(commandAllocators_[frameIndex_].Get(), nullptr),
        "ID3D12GraphicsCommandList::Reset");

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = renderTargets_[frameIndex_].Get();
    toRenderTarget.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &toRenderTarget);

    const D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView = OffsetDescriptor(
        rtvHeap_->GetCPUDescriptorHandleForHeapStart(),
        frameIndex_,
        rtvDescriptorSize_);
    const D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView =
        dsvHeap_->GetCPUDescriptorHandleForHeapStart();

    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissorRectangle_);
    commandList_->OMSetRenderTargets(
        1,
        &renderTargetView,
        FALSE,
        &depthStencilView);

    const float color[]{
        clearColor.red,
        clearColor.green,
        clearColor.blue,
        clearColor.alpha};
    commandList_->ClearRenderTargetView(
        renderTargetView,
        color,
        0,
        nullptr);
    commandList_->ClearDepthStencilView(
        depthStencilView,
        D3D12_CLEAR_FLAG_DEPTH,
        1.0F,
        0,
        0,
        nullptr);

    const std::uint64_t renderIndex = nextRenderIndex_++;
    MeshRenderingBackend::BeginFrame(*meshRenderSystem_, frameIndex_);
    TextRenderingBackend::BeginFrame(*textRenderSystem_, frameIndex_, renderIndex);
    static_cast<D3D12Visual2DRenderer&>(*visual2DRenderSystem_).BeginScreenFrame(renderIndex, frameIndex_);
    imageReadbacks_->BeginFrame(frameIndex_, renderIndex, *renderTargets_[frameIndex_].Get());
    frameOpen_ = true;
    RenderContext context;
    frameContext_.commandList = commandList_.Get();
    frameContext_.viewport = viewport_;
    frameContext_.scissorRectangle = scissorRectangle_;
    context.width = width_;
    context.height = height_;
    context.frameIndex = frameIndex_;
    context.renderIndex = renderIndex;
    frameContext_.renderTargetView = renderTargetView;
    frameContext_.depthStencilView = depthStencilView;
    context.meshRendering = meshRenderSystem_.get();
    context.textRendering = textRenderSystem_.get();
    context.visual2DRendering = visual2DRenderSystem_.get();
    frameContext_.renderIndex = renderIndex;
    frameContext_.imageReadbacks = imageReadbacks_.get();
    frameContext_.Attach(context);
    return context;
}

void D3D12Renderer::EndFrame()
{
    if (!frameOpen_)
    {
        throw std::logic_error("No D3D12 frame is open.");
    }

    // Client scenes only submit high-level MeshInstances.  The renderer
    // resolves material pipelines, batches instances, and records draws
    // before the back buffer transitions to PRESENT.
    MeshRenderingBackend::Flush(*meshRenderSystem_, *commandList_.Get());
    static_cast<D3D12Visual2DRenderer&>(*visual2DRenderSystem_).DrawScreen(
        *commandList_.Get(), frameIndex_, width_, height_);
    TextRenderingBackend::Flush(
        *textRenderSystem_,
        *commandList_.Get(),
        width_,
        height_);
    imageReadbacks_->RecordCopy(*commandList_.Get());

    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource = renderTargets_[frameIndex_].Get();
    toPresent.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPresent.Transition.StateBefore =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &toPresent);

    // Present interval zero keeps presentation from becoming the update
    // limiter.  Tearing is requested only when DXGI reports support.
    ThrowIfFailed(
        commandList_->Close(),
        "ID3D12GraphicsCommandList::Close");
    ID3D12CommandList* commandLists[]{commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);
    imageReadbacks_->MarkSubmitted();

    const UINT presentFlags = tearingSupported_
        ? DXGI_PRESENT_ALLOW_TEARING
        : 0;
    ThrowIfFailed(
        swapChain_->Present(0, presentFlags),
        "IDXGISwapChain4::Present");

    // Remember which GPU completion value makes this frame resource safe
    // to reuse on a future BeginFrame.
    const std::uint64_t signalValue = nextFenceValue_++;
    ThrowIfFailed(
        commandQueue_->Signal(fence_.Get(), signalValue),
        "ID3D12CommandQueue::Signal");
    frameFenceValues_[frameIndex_] = signalValue;
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    frameOpen_ = false;
}

void D3D12Renderer::WaitForGpu()
{
    if (commandQueue_ == nullptr || fence_ == nullptr)
    {
        return;
    }

    // Full-queue waits are reserved for resize/shutdown, when resource
    // destruction or swap-chain replacement must be globally safe.
    const std::uint64_t signalValue = nextFenceValue_++;
    ThrowIfFailed(
        commandQueue_->Signal(fence_.Get(), signalValue),
        "ID3D12CommandQueue::Signal");

    if (fence_->GetCompletedValue() < signalValue)
    {
        ThrowIfFailed(
            fence_->SetEventOnCompletion(signalValue, fenceEvent_),
            "ID3D12Fence::SetEventOnCompletion");
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    if (imageReadbacks_)
        imageReadbacks_->CompleteAll();
}

ID3D12Device* D3D12Renderer::Device() const noexcept
{
    return device_.Get();
}

MeshRenderSystem& D3D12Renderer::MeshRendering() const noexcept
{
    return *meshRenderSystem_;
}

TextRenderSystem& D3D12Renderer::TextRendering() const noexcept
{
    return *textRenderSystem_;
}

Visual2DRenderSystem& D3D12Renderer::Visual2DRendering() const noexcept
{
    return *visual2DRenderSystem_;
}

DXGI_FORMAT D3D12Renderer::RenderTargetFormat() const noexcept
{
    return renderTargetFormat_;
}

DXGI_FORMAT D3D12Renderer::DepthStencilFormat() const noexcept
{
    return depthStencilFormat_;
}

bool D3D12Renderer::SupportsTearing() const noexcept
{
    return tearingSupported_;
}

void D3D12Renderer::CreateDeviceAndFactory()
{
    UINT factoryFlags = 0;

#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(
            IID_PPV_ARGS(debugController.ReleaseAndGetAddressOf()))))
    {
        debugController->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ThrowIfFailed(
        CreateDXGIFactory2(
            factoryFlags,
            IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf())),
        "CreateDXGIFactory2");

    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory_->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing))))
    {
        tearingSupported_ = allowTearing == TRUE;
    }

    const auto adapter = SelectAdapter(*factory_.Get());
    ThrowIfFailed(
        D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device_.ReleaseAndGetAddressOf())),
        "D3D12CreateDevice");
}

void D3D12Renderer::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    ThrowIfFailed(
        device_->CreateCommandQueue(
            &queueDescription,
            IID_PPV_ARGS(commandQueue_.ReleaseAndGetAddressOf())),
        "ID3D12Device::CreateCommandQueue");

    for (auto& allocator : commandAllocators_)
    {
        ThrowIfFailed(
            device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommandAllocator");
    }

    ThrowIfFailed(
        device_->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            commandAllocators_[0].Get(),
            nullptr,
            IID_PPV_ARGS(commandList_.ReleaseAndGetAddressOf())),
        "ID3D12Device::CreateCommandList");
    ThrowIfFailed(
        commandList_->Close(),
        "ID3D12GraphicsCommandList::Close");

    ThrowIfFailed(
        device_->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf())),
        "ID3D12Device::CreateFence");

    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr)
    {
        throw std::runtime_error("CreateEventW failed for the D3D12 fence.");
    }
}

void D3D12Renderer::CreateSwapChain(const HWND window)
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width_;
    description.Height = height_;
    description.Format = renderTargetFormat_;
    description.Stereo = FALSE;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = D3D12Detail::FrameCount;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    description.Flags = tearingSupported_
        ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        : 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(
        factory_->CreateSwapChainForHwnd(
            commandQueue_.Get(),
            window,
            &description,
            nullptr,
            nullptr,
            swapChain.ReleaseAndGetAddressOf()),
        "IDXGIFactory6::CreateSwapChainForHwnd");
    ThrowIfFailed(
        factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER),
        "IDXGIFactory6::MakeWindowAssociation");
    ThrowIfFailed(
        swapChain.As(&swapChain_),
        "Query IDXGISwapChain4");

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

void D3D12Renderer::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
    rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescription.NumDescriptors = D3D12Detail::FrameCount;
    ThrowIfFailed(
        device_->CreateDescriptorHeap(
            &rtvDescription,
            IID_PPV_ARGS(rtvHeap_.ReleaseAndGetAddressOf())),
        "Create RTV descriptor heap");
    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvDescription{};
    dsvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDescription.NumDescriptors = 1;
    ThrowIfFailed(
        device_->CreateDescriptorHeap(
            &dsvDescription,
            IID_PPV_ARGS(dsvHeap_.ReleaseAndGetAddressOf())),
        "Create DSV descriptor heap");
}

void D3D12Renderer::CreateRenderTargets()
{
    const D3D12_CPU_DESCRIPTOR_HANDLE start =
        rtvHeap_->GetCPUDescriptorHandleForHeapStart();

    for (std::uint32_t index = 0; index < D3D12Detail::FrameCount; ++index)
    {
        ThrowIfFailed(
            swapChain_->GetBuffer(
                index,
                IID_PPV_ARGS(renderTargets_[index].ReleaseAndGetAddressOf())),
            "IDXGISwapChain4::GetBuffer");
        const D3D12_CPU_DESCRIPTOR_HANDLE handle =
            OffsetDescriptor(start, index, rtvDescriptorSize_);
        device_->CreateRenderTargetView(
            renderTargets_[index].Get(),
            nullptr,
            handle);
    }
}

void D3D12Renderer::CreateDepthStencil()
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width_;
    description.Height = height_;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = depthStencilFormat_;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = depthStencilFormat_;
    clearValue.DepthStencil.Depth = 1.0F;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(depthStencil_.ReleaseAndGetAddressOf())),
        "Create depth-stencil resource");

    D3D12_DEPTH_STENCIL_VIEW_DESC viewDescription{};
    viewDescription.Format = depthStencilFormat_;
    viewDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(
        depthStencil_.Get(),
        &viewDescription,
        dsvHeap_->GetCPUDescriptorHandleForHeapStart());
}

void D3D12Renderer::UpdateViewport() noexcept
{
    viewport_.TopLeftX = 0.0F;
    viewport_.TopLeftY = 0.0F;
    viewport_.Width = static_cast<float>(width_);
    viewport_.Height = static_cast<float>(height_);
    viewport_.MinDepth = 0.0F;
    viewport_.MaxDepth = 1.0F;

    scissorRectangle_.left = 0;
    scissorRectangle_.top = 0;
    scissorRectangle_.right = static_cast<LONG>(width_);
    scissorRectangle_.bottom = static_cast<LONG>(height_);
}

void D3D12Renderer::WaitForFrame(const std::uint32_t frameIndex)
{
    const std::uint64_t fenceValue = frameFenceValues_[frameIndex];
    if (fenceValue == 0 || fence_->GetCompletedValue() >= fenceValue)
    {
        return;
    }

    ThrowIfFailed(
        fence_->SetEventOnCompletion(fenceValue, fenceEvent_),
        "ID3D12Fence::SetEventOnCompletion");
    WaitForSingleObject(fenceEvent_, INFINITE);
}

} // namespace PhantomCore
