#include "Renderer/D3D12Renderer.h"
#include "Mesh/D3D12MeshRendering.h"
#include "Text/D3D12TextRendering.h"
#include "Renderer/FrameImageReadbackQueue.h"
#include "Renderer/D3D12Error.h"
#include "Renderer/D3D12ResourceDescriptions.h"
#include "Renderer/GraphicsDeviceDiagnostics.h"

#include "Mesh/MeshRendering.h"
#include "Text/TextRendering.h"
#include "Visual2D/D3D12Visual2DRenderer.h"

#include <d3d12sdklayers.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>


namespace PhantomCore
{
namespace
{
    using D3D12Detail::ThrowIfFailed;

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

    [[nodiscard]] bool CanCreateDevice(IDXGIAdapter1& adapter) noexcept
    {
        return SUCCEEDED(D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<IDXGIAdapter1> WarpAdapter(IDXGIFactory6& factory)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        ThrowIfFailed(
            factory.EnumWarpAdapter(
                IID_PPV_ARGS(warpAdapter.ReleaseAndGetAddressOf())),
            "IDXGIFactory6::EnumWarpAdapter");
        ThrowIfFailed(
            warpAdapter.As(&adapter),
            "Query WARP IDXGIAdapter1");
        return adapter;
    }

    // ASCII letters compared without case; adapter names are ASCII in practice.
    [[nodiscard]] bool NameContains(const wchar_t* name, const std::string& part)
    {
        std::string folded;
        for (; *name != 0; ++name)
        {
            const bool ascii = *name < 128;
            folded.push_back(ascii ? static_cast<char>(std::tolower(static_cast<int>(*name))) : '?');
        }
        std::string wanted;
        for (const char character : part)
            wanted.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        return !wanted.empty() && folded.find(wanted) != std::string::npos;
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<IDXGIAdapter1> NamedAdapter(IDXGIFactory6& factory, const std::string& name)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0; factory.EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++index)
        {
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(adapter->GetDesc1(&description)) && NameContains(description.Description, name))
                return adapter;
        }
        return nullptr;
    }

    // The adapter a GraphicsAdapterRequest names, or null when there is none.
    [[nodiscard]] Microsoft::WRL::ComPtr<IDXGIAdapter1> RequestedAdapter(
        IDXGIFactory6& factory,
        const GraphicsDeviceDiagnosticsConfig& config)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        switch (config.adapter)
        {
        case GraphicsAdapterRequest::Warp:
            return WarpAdapter(factory);
        case GraphicsAdapterRequest::Index:
            if (FAILED(factory.EnumAdapters1(config.adapterIndex, adapter.GetAddressOf())))
                return nullptr;
            return adapter;
        case GraphicsAdapterRequest::Name:
            return NamedAdapter(factory, config.adapterName);
        case GraphicsAdapterRequest::Default:
            break;
        }
        return nullptr;
    }

    [[nodiscard]] std::string DescribeRequest(const GraphicsDeviceDiagnosticsConfig& config)
    {
        switch (config.adapter)
        {
        case GraphicsAdapterRequest::Warp:
            return "WARP";
        case GraphicsAdapterRequest::Index:
            return "index " + std::to_string(config.adapterIndex);
        case GraphicsAdapterRequest::Name:
            return "name \"" + config.adapterName + "\"";
        case GraphicsAdapterRequest::Default:
            break;
        }
        return "default";
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

            if (CanCreateDevice(*adapter.Get()))
            {
                return adapter;
            }
        }

        return WarpAdapter(factory);
    }

    void Name(ID3D12Object& object, const std::wstring& name) noexcept
    {
        D3D12Detail::SetDebugName(object, name.c_str());
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

    if (validationMessages_.Active() && device_ != nullptr)
    {
        ReleaseAllButDevice();
        validationMessages_.Finish(*device_.Get());
    }

    if (frameLatencyWait_ != nullptr)
    {
        CloseHandle(frameLatencyWait_);
        frameLatencyWait_ = nullptr;
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
    const std::uint32_t height,
    const EngineDiagnosticsConfig& diagnostics)
{
    window_ = window;
    width_ = width;
    height_ = height;
    sink_ = diagnostics.sink;
    graphics_ = diagnostics.graphics;
    swapChainBuffers_ = std::clamp<std::uint32_t>(graphics_.swapChainBuffers, D3D12Detail::FrameCount, MaximumSwapChainBuffers);
    // DRED only watches devices created after it was switched on.
    deviceRemovedData_ = diagnostics.graphicsDeviceRemovedData && D3D12Detail::EnableDeviceRemovedExtendedData();

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

    // The flags must match the ones the swap chain was created with.
    ThrowIfFailed(
        swapChain_->ResizeBuffers(
            swapChainBuffers_,
            width,
            height,
            renderTargetFormat_,
            SwapChainFlags()),
        "IDXGISwapChain4::ResizeBuffers");

    width_ = width;
    height_ = height;
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
    WaitForSwapChain();
    WaitForFrame(frameIndex_);
    backBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();
    imageReadbacks_->CompleteFrame(frameIndex_);
    ThrowIfFailed(
        commandAllocators_[frameIndex_]->Reset(),
        "ID3D12CommandAllocator::Reset");
    ThrowIfFailed(
        commandList_->Reset(commandAllocators_[frameIndex_].Get(), nullptr),
        "ID3D12GraphicsCommandList::Reset");

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = renderTargets_[backBufferIndex_].Get();
    toRenderTarget.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &toRenderTarget);

    const D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView = OffsetDescriptor(
        rtvHeap_->GetCPUDescriptorHandleForHeapStart(),
        backBufferIndex_,
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
    D3D12Detail::RecordedDrawCalls() = 0;
    MeshRenderingBackend::BeginFrame(*meshRenderSystem_, frameIndex_);
    TextRenderingBackend::BeginFrame(*textRenderSystem_, frameIndex_, renderIndex);
    static_cast<D3D12Visual2DRenderer&>(*visual2DRenderSystem_).BeginScreenFrame(renderIndex, frameIndex_);
    imageReadbacks_->BeginFrame(frameIndex_, renderIndex, *renderTargets_[backBufferIndex_].Get());
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
    lastFrameDrawCalls_ = D3D12Detail::RecordedDrawCalls();

    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource = renderTargets_[backBufferIndex_].Get();
    toPresent.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPresent.Transition.StateBefore =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &toPresent);

    // Present interval zero keeps presentation from becoming the update
    // limiter.  Tearing is requested only when DXGI reports support and the
    // diagnostics configuration did not turn it off.
    ThrowIfFailed(
        commandList_->Close(),
        "ID3D12GraphicsCommandList::Close");
    ID3D12CommandList* commandLists[]{commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);
    imageReadbacks_->MarkSubmitted();

    const UINT presentFlags = (SwapChainFlags() & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0
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
    frameIndex_ = (frameIndex_ + 1) % D3D12Detail::FrameCount;
    frameOpen_ = false;
    validationMessages_.Forward();
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

std::vector<std::string> D3D12Renderer::StartupRecord() const
{
    std::vector<std::string> lines;
    if (adapter_ == nullptr)
        return lines;
    lines.push_back("graphics adapter: " + D3D12Detail::DescribeAdapter(*adapter_.Get()));
    lines.push_back("swap chain: " + DescribeSwapChain());
    lines.push_back(std::string("device removed data: ") + (deviceRemovedData_ ? "on" : "off"));
    lines.push_back(
        "graphics options: validation " + D3D12Detail::DescribeValidation(validation_) + ", adapter " + adapterRequest_);
    return lines;
}

// "1280x720, 2 buffers, tearing allowed" and, when asked for, ", waitable (latency 1)".
std::string D3D12Renderer::DescribeSwapChain() const
{
    const char* tearing = "not supported";
    if (tearingSupported_)
        tearing = graphics_.allowTearing ? "allowed" : "off by setting";
    std::string line = std::to_string(width_) + "x" + std::to_string(height_) + ", " +
        std::to_string(swapChainBuffers_) + " buffers, tearing " + tearing;
    if (frameLatencyWait_ != nullptr)
        line += ", waitable (latency 1)";
    return line;
}

UINT D3D12Renderer::SwapChainFlags() const noexcept
{
    UINT flags = 0;
    if (tearingSupported_ && graphics_.allowTearing)
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (graphics_.waitableSwapChain)
        flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    return flags;
}

// A waitable swap chain signals when it can take another frame. The wait is
// bounded so a stuck presentation cannot hold the main loop forever.
void D3D12Renderer::WaitForSwapChain() const noexcept
{
    constexpr DWORD MaximumWaitMilliseconds = 1000;
    if (frameLatencyWait_ != nullptr)
        static_cast<void>(WaitForSingleObjectEx(frameLatencyWait_, MaximumWaitMilliseconds, TRUE));
}

void D3D12Renderer::ReleaseAllButDevice() noexcept
{
    // The same order the members would be destroyed in.
    imageReadbacks_.reset();
    visual2DRenderSystem_.reset();
    textRenderSystem_.reset();
    meshRenderSystem_.reset();
    fence_.Reset();
    commandList_.Reset();
    for (auto& allocator : commandAllocators_)
        allocator.Reset();
    depthStencil_.Reset();
    for (auto& target : renderTargets_)
        target.Reset();
    dsvHeap_.Reset();
    rtvHeap_.Reset();
    swapChain_.Reset();
    commandQueue_.Reset();
}

GraphicsMemoryInfo D3D12Renderer::MemoryInfo() const noexcept
{
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    if (adapter_ == nullptr || FAILED(adapter_.As(&adapter)))
        return {};
    return D3D12Detail::QueryGraphicsMemory(adapter.Get());
}

std::optional<GraphicsDeviceLossReport> D3D12Renderer::DeviceLossReport(
    const char* operation,
    const std::uint32_t resultCode) const
{
    if (device_ == nullptr)
        return std::nullopt;
    const HRESULT reason = device_->GetDeviceRemovedReason();
    // A failed call that named the loss is reported even if the device gives no reason.
    if (reason == S_OK && operation == nullptr)
        return std::nullopt;

    GraphicsDeviceLossReport report;
    report.operation = operation != nullptr ? operation : "";
    report.resultCode = resultCode;
    report.removedReason = static_cast<std::uint32_t>(reason);
    report.removedReasonName = D3D12Detail::DeviceRemovedReasonName(reason);
    report.renderIndex = nextRenderIndex_ - 1;
    report.width = width_;
    report.height = height_;
    report.lastFrameDrawCalls = lastFrameDrawCalls_;
    report.memory = MemoryInfo();
    report.extendedDataEnabled = deviceRemovedData_;
    if (deviceRemovedData_)
        report.extendedData = D3D12Detail::ReadDeviceRemovedExtendedData(*device_.Get());
    return report;
}

void D3D12Renderer::RemoveDeviceForDiagnosticsTest()
{
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    if (device_ == nullptr || FAILED(device_.As(&device)))
        throw std::runtime_error("This Windows version cannot remove the D3D12 device on request.");
    device->RemoveDevice();
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
    // Validation must be on before the device exists. Debug builds keep the
    // debug layer they always had.
    validation_ = D3D12Detail::EnableValidation(graphics_.validation);
    const UINT factoryFlags = validation_.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    HRESULT factoryCreated = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
    // The DXGI debug part may be missing even where the D3D12 layer exists.
    if (FAILED(factoryCreated) && factoryFlags != 0)
        factoryCreated = CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
    ThrowIfFailed(factoryCreated, "CreateDXGIFactory2");

    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory_->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing))))
    {
        tearingSupported_ = allowTearing == TRUE;
    }

    // A requested adapter that is missing or cannot make a device falls back
    // to the normal choice, and the start record says so.
    adapterRequest_ = DescribeRequest(graphics_);
    adapter_ = RequestedAdapter(*factory_.Get(), graphics_);
    const bool requested = graphics_.adapter != GraphicsAdapterRequest::Default;
    if (adapter_ != nullptr && !CanCreateDevice(*adapter_.Get()))
    {
        adapter_.Reset();
        adapterRequest_ += " (no device, default used)";
    }
    else if (adapter_ == nullptr && requested)
    {
        adapterRequest_ += " (not found, default used)";
    }
    if (adapter_ == nullptr)
        adapter_ = SelectAdapter(*factory_.Get());
    ThrowIfFailed(
        D3D12CreateDevice(
            adapter_.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device_.ReleaseAndGetAddressOf())),
        "D3D12CreateDevice");

    // Only a Client that asked for validation gets its messages as records.
    if (graphics_.validation != GraphicsValidation::Off && validation_.debugLayer)
        validationMessages_.Attach(*device_.Get(), sink_, graphics_.validationMessageLimit);
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
    // The names appear in the device-removed data and in graphics debuggers.
    static_cast<void>(commandQueue_->SetName(L"Main queue"));

    for (std::uint32_t slot = 0; slot < D3D12Detail::FrameCount; ++slot)
    {
        auto& allocator = commandAllocators_[slot];
        ThrowIfFailed(
            device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommandAllocator");
        Name(*allocator.Get(), L"Frame allocator " + std::to_wstring(slot));
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
    static_cast<void>(commandList_->SetName(L"Frame"));

    ThrowIfFailed(
        device_->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf())),
        "ID3D12Device::CreateFence");
    Name(*fence_.Get(), L"Frame fence");

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
    description.BufferCount = swapChainBuffers_;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    description.Flags = SwapChainFlags();

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

    if (graphics_.waitableSwapChain)
    {
        ThrowIfFailed(
            swapChain_->SetMaximumFrameLatency(1),
            "IDXGISwapChain4::SetMaximumFrameLatency");
        frameLatencyWait_ = swapChain_->GetFrameLatencyWaitableObject();
    }
    backBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

void D3D12Renderer::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
    rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescription.NumDescriptors = swapChainBuffers_;
    ThrowIfFailed(
        device_->CreateDescriptorHeap(
            &rtvDescription,
            IID_PPV_ARGS(rtvHeap_.ReleaseAndGetAddressOf())),
        "Create RTV descriptor heap");
    Name(*rtvHeap_.Get(), L"Back buffer views");
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
    Name(*dsvHeap_.Get(), L"Depth stencil view");
}

void D3D12Renderer::CreateRenderTargets()
{
    const D3D12_CPU_DESCRIPTOR_HANDLE start =
        rtvHeap_->GetCPUDescriptorHandleForHeapStart();

    for (std::uint32_t index = 0; index < swapChainBuffers_; ++index)
    {
        ThrowIfFailed(
            swapChain_->GetBuffer(
                index,
                IID_PPV_ARGS(renderTargets_[index].ReleaseAndGetAddressOf())),
            "IDXGISwapChain4::GetBuffer");
        Name(*renderTargets_[index].Get(), L"Back buffer " + std::to_wstring(index));
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
    Name(*depthStencil_.Get(), L"Depth stencil");

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
