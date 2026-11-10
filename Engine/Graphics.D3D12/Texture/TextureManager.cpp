#include "Texture/TextureManager.h"
#include "Renderer/D3D12Error.h"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;


namespace
{
    constexpr std::uint32_t DescriptorHeapCapacity = 1024;

    [[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(
        const std::uint64_t byteCount) noexcept
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = byteCount;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return description;
    }

    [[nodiscard]] D3D12_RESOURCE_DESC TextureDescription(
        const std::uint32_t width,
        const std::uint32_t height) noexcept
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        return description;
    }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE OffsetCpuDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE start,
        const std::uint32_t index,
        const std::uint32_t descriptorSize) noexcept
    {
        start.ptr += static_cast<SIZE_T>(index) * descriptorSize;
        return start;
    }

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE OffsetGpuDescriptor(
        D3D12_GPU_DESCRIPTOR_HANDLE start,
        const std::uint32_t index,
        const std::uint32_t descriptorSize) noexcept
    {
        start.ptr += static_cast<UINT64>(index) * descriptorSize;
        return start;
    }

    // Backend allocations live only in this translation unit. Public
    // handles expose metadata and lifetime without leaking D3D12 state or
    // privileged access to private fields.
    class D3D12TextureSet final : public TextureSet
    {
    public:
        void Append(
            const TextureInfo info,
            ComPtr<ID3D12Resource> resource)
        {
            AppendInfo(info);
            resources.push_back(std::move(resource));
        }

        std::vector<ComPtr<ID3D12Resource>> resources;
        // Frame and upload owners retain this lease through their fences.
        std::shared_ptr<const void> descriptorAllocation;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorStart{};
    };

    class D3D12RenderTargetTexture final : public RenderTargetTexture
    {
    public:
        [[nodiscard]] const TextureSetHandle& Textures()
            const noexcept override
        {
            return textures;
        }

        [[nodiscard]] std::uint32_t Width() const noexcept override
        {
            return width;
        }

        [[nodiscard]] std::uint32_t Height() const noexcept override
        {
            return height;
        }

        TextureSetHandle textures;
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        D3D12_RESOURCE_STATES state{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    [[nodiscard]] const D3D12TextureSet& RequireTextureSet(
        const TextureSetHandle& textureSet)
    {
        const auto* concrete = dynamic_cast<const D3D12TextureSet*>(
            textureSet.get());
        if (concrete == nullptr)
        {
            throw std::invalid_argument(
                "The texture set was not created by this D3D12 backend.");
        }
        return *concrete;
    }

    [[nodiscard]] D3D12TextureSet& RequireMutableTextureSet(
        const TextureSetHandle& textureSet)
    {
        return const_cast<D3D12TextureSet&>(
            RequireTextureSet(textureSet));
    }

    [[nodiscard]] D3D12TextureSet& RequireTextureSet(
        TextureSet& textureSet)
    {
        auto* concrete = dynamic_cast<D3D12TextureSet*>(&textureSet);
        if (concrete == nullptr)
        {
            throw std::invalid_argument(
                "The texture set was not created by this D3D12 backend.");
        }
        return *concrete;
    }

    [[nodiscard]] D3D12RenderTargetTexture& RequireRenderTarget(
        const RenderTargetTextureHandle& target)
    {
        auto* concrete = dynamic_cast<D3D12RenderTargetTexture*>(
            target.get());
        if (concrete == nullptr)
        {
            throw std::invalid_argument(
                "The render target was not created by this D3D12 backend.");
        }
        return *concrete;
    }

}

struct TextureManager::DescriptorBlockPool final
{
    struct Lease final
    {
        std::shared_ptr<DescriptorBlockPool> pool;
        std::uint32_t offset{};
        ~Lease()
        {
            std::lock_guard lock(pool->mutex);
            pool->used[offset / MaxTexturesPerSet] = false;
        }
    };

    std::shared_ptr<Lease> Acquire(const std::shared_ptr<DescriptorBlockPool>& owner)
    {
        std::lock_guard lock(mutex);
        const auto available = std::ranges::find(used, false);
        if (available == used.end())
            throw std::runtime_error("The texture descriptor heap has no free material block.");
        auto lease = std::make_shared<Lease>();
        lease->pool = owner;
        lease->offset = static_cast<std::uint32_t>(available - used.begin()) * MaxTexturesPerSet;
        *available = true;
        return lease;
    }

    // A lease may outlive its manager, or be released on another thread.
    // It always returns to its original heap generation.
    ComPtr<ID3D12DescriptorHeap> heap;
    std::mutex mutex;
    std::array<bool, DescriptorHeapCapacity / MaxTexturesPerSet> used{};
};

struct TextureManager::DecodedImage final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> pixels;
};

TextureSet::~TextureSet() = default;

void TextureSet::AppendInfo(const TextureInfo info)
{
    textureInfo_.push_back(info);
}

std::size_t TextureSet::Size() const noexcept
{
    return textureInfo_.size();
}

const TextureInfo& TextureSet::Info(const std::size_t index) const
{
    if (index >= textureInfo_.size())
    {
        throw std::out_of_range("TextureSet index is out of range.");
    }
    return textureInfo_[index];
}

UvTransform TextureSet::MakeCoverUvTransform(
    const std::size_t index,
    const float targetAspectRatio) const
{
    if (targetAspectRatio <= 0.0F)
    {
        throw std::invalid_argument(
            "The target UV aspect ratio must be positive.");
    }

    const TextureInfo& info = Info(index);
    const float sourceAspectRatio =
        static_cast<float>(info.width) /
        static_cast<float>(info.height);

    UvTransform transform{};
    if (sourceAspectRatio > targetAspectRatio)
    {
        transform.scale.x =
            targetAspectRatio / sourceAspectRatio;
        transform.offset.x =
            (1.0F - transform.scale.x) * 0.5F;
    }
    else if (sourceAspectRatio < targetAspectRatio)
    {
        transform.scale.y =
            sourceAspectRatio / targetAspectRatio;
        transform.offset.y =
            (1.0F - transform.scale.y) * 0.5F;
    }
    return transform;
}

RenderTargetTexture::~RenderTargetTexture() = default;

void TextureManager::PollPreparation()
{
    if (!preparation_.valid() || preparation_.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        return;
    try
    {
        preparedImages_.merge(preparation_.get());
    }
    catch (const std::exception &error)
    {
        preparationError_ = error.what();
    }
}

bool TextureManager::PrepareImages(std::span<const std::filesystem::path> paths, std::string &error)
{
    if (!initialized_)
        throw std::logic_error("Image preparation requires an initialized texture manager.");
    PollPreparation();
    error = std::exchange(preparationError_, {});
    if (!error.empty())
        return true;
    if (preparation_.valid())
        return false;
    std::vector<std::filesystem::path> missing;
    for (const auto &path : paths)
        if (!preparedImages_.contains(path.lexically_normal()))
            missing.push_back(path.lexically_normal());
    if (missing.empty())
        return true;
    preparation_ = std::async(std::launch::async,
                              [paths = std::move(missing)]
                              {
                                  const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                                  D3D12Detail::ThrowIfFailed(result, "Initialize image preparation COM apartment");
                                  struct Apartment
                                  {
                                      ~Apartment()
                                      {
                                          CoUninitialize();
                                      }
                                  } apartment;
                                  std::map<std::filesystem::path, std::shared_ptr<DecodedImage>> decoded;
                                  for (const auto &path : paths)
                                      decoded.emplace(path, std::make_shared<DecodedImage>(DecodeImage(path)));
                                  return decoded;
                              });
    return false;
}

TextureManager::DecodedImage TextureManager::TakePreparedImage(const std::filesystem::path &path)
{
    PollPreparation();
    if (auto cached = preparedImages_.find(path.lexically_normal()); cached != preparedImages_.end())
    {
        auto image = std::move(*cached->second);
        preparedImages_.erase(cached);
        return image;
    }
    return DecodeImage(path);
}

TextureManager::~TextureManager()
{
    Shutdown();
}

void TextureManager::Initialize(
    ID3D12Device& device,
    ID3D12CommandQueue& commandQueue)
{
    if (initialized_)
    {
        throw std::logic_error(
            "TextureManager is already initialized.");
    }

    device_ = &device;
    commandQueue_ = &commandQueue;
    try
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
        heapDescription.Type =
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDescription.NumDescriptors = DescriptorHeapCapacity;
        heapDescription.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12Detail::ThrowIfFailed(
            device.CreateDescriptorHeap(
                &heapDescription,
                IID_PPV_ARGS(
                    descriptorHeap_.ReleaseAndGetAddressOf())),
            "Create texture descriptor heap");
        descriptorSize_ = device.GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        descriptorBlocks_ = std::make_shared<DescriptorBlockPool>();
        descriptorBlocks_->heap = descriptorHeap_;

        D3D12Detail::ThrowIfFailed(
            device.CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(
                    uploadAllocator_.ReleaseAndGetAddressOf())),
            "Create texture upload command allocator");
        D3D12Detail::ThrowIfFailed(
            device.CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                uploadAllocator_.Get(),
                nullptr,
                IID_PPV_ARGS(
                    uploadCommandList_.ReleaseAndGetAddressOf())),
            "Create texture upload command list");
        D3D12Detail::ThrowIfFailed(
            uploadCommandList_->Close(),
            "Close initial texture upload command list");

        D3D12Detail::ThrowIfFailed(
            device.CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(uploadFence_.ReleaseAndGetAddressOf())),
            "Create texture upload fence");
        uploadFenceEvent_ =
            CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (uploadFenceEvent_ == nullptr)
        {
            throw std::runtime_error(
                "CreateEventW failed for texture uploads.");
        }

        initialized_ = true;
    }
    catch (...)
    {
        Shutdown();
        throw;
    }
}

void TextureManager::Shutdown() noexcept
{
    // The worker owns only copied paths. Join at teardown, never in a frame,
    // so a subsequent Initialize cannot consume an earlier preparation job.
    if (preparation_.valid())
    {
        try
        {
            static_cast<void>(preparation_.get());
        }
        catch (...)
        {
        }
    }
    if (!pendingUploads_.empty() && pendingUploads_.back().fence == 0 && commandQueue_ && uploadFence_)
    {
        // A failed submission signal must not leave an unreachable fence
        // that Shutdown waits on forever. Retry once while resources live.
        const auto fence = nextUploadFenceValue_++;
        if (SUCCEEDED(commandQueue_->Signal(uploadFence_.Get(), fence)))
            pendingUploads_.back().fence = fence;
    }
    if (!pendingUploads_.empty() && uploadFence_ && uploadFenceEvent_ && pendingUploads_.back().fence != 0 &&
        uploadFence_->GetCompletedValue() < pendingUploads_.back().fence)
    {
        if (SUCCEEDED(uploadFence_->SetEventOnCompletion(pendingUploads_.back().fence, uploadFenceEvent_)))
            WaitForSingleObject(uploadFenceEvent_, INFINITE);
    }
    pendingUploads_.clear();
    preparedImages_.clear();
    preparationError_.clear();
    if (uploadFenceEvent_ != nullptr)
    {
        CloseHandle(uploadFenceEvent_);
        uploadFenceEvent_ = nullptr;
    }

    uploadFence_.Reset();
    uploadCommandList_.Reset();
    uploadAllocator_.Reset();
    descriptorHeap_.Reset();
    device_ = nullptr;
    commandQueue_ = nullptr;
    nextUploadFenceValue_ = 1;
    descriptorSize_ = 0;
    descriptorBlocks_.reset();
    initialized_ = false;
}

TextureSetHandle TextureManager::LoadTextureSet(
    const std::span<const std::filesystem::path> paths)
{
    ValidateTextureSetRequest(paths);

    // Decode first so a bad file cannot leave an open D3D12 upload list.
    std::vector<DecodedImage> decodedImages;
    decodedImages.reserve(paths.size());
    for (const std::filesystem::path& path : paths)
    {
        decodedImages.push_back(TakePreparedImage(path));
    }

    auto textureSet = std::make_shared<D3D12TextureSet>();
    textureSet->resources.reserve(decodedImages.size());
    std::vector<ComPtr<ID3D12Resource>> uploadBuffers;
    uploadBuffers.reserve(decodedImages.size());
    const D3D12_CPU_DESCRIPTOR_HANDLE cpuBlockStart =
        InitializeDescriptorBlock(*textureSet);
    BeginUploadCommands();

    try
    {
        for (std::size_t index = 0;
             index < decodedImages.size();
             ++index)
        {
            RecordTextureUpload(
                decodedImages[index],
                index,
                cpuBlockStart,
                *textureSet,
                uploadBuffers);
        }

        D3D12Detail::ThrowIfFailed(
            uploadCommandList_->Close(),
            "Close texture upload command list");
    }
    catch (...)
    {
        // The list was never submitted, so closing it makes a later Reset
        // legal without requiring a fence.
        uploadCommandList_->Close();
        throw;
    }

    ExecuteUpload(std::move(uploadBuffers), textureSet);
    return textureSet;
}

TextureSetHandle TextureManager::AppendTexture(
    const TextureSetHandle& textureSet,
    const std::filesystem::path& path)
{
    if (!initialized_ || device_ == nullptr || commandQueue_ == nullptr)
    {
        throw std::logic_error(
            "TextureManager must be initialized before appending images.");
    }
    if (textureSet == nullptr || path.empty())
    {
        throw std::invalid_argument(
            "Appending a texture requires an existing set and path.");
    }
    if (textureSet->Size() >= MaxTexturesPerSet)
    {
        throw std::length_error(
            "A texture descriptor table cannot contain more than 64 images.");
    }

    // Decode before opening the upload list so invalid user assets cannot
    // leave the reusable allocator in a recording state.
    const DecodedImage decodedImage = TakePreparedImage(path);
    D3D12TextureSet& mutableSet =
        RequireMutableTextureSet(textureSet);
    BeginUploadCommands();
    std::vector<ComPtr<ID3D12Resource>> uploadBuffers;
    uploadBuffers.reserve(1);

    try
    {
        RecordTextureUpload(
            decodedImage,
            mutableSet.Size(),
            mutableSet.cpuDescriptorStart,
            mutableSet,
            uploadBuffers);
        D3D12Detail::ThrowIfFailed(
            uploadCommandList_->Close(),
            "Close appended texture upload command list");
    }
    catch (...)
    {
        uploadCommandList_->Close();
        throw;
    }

    ExecuteUpload(std::move(uploadBuffers), textureSet);
    return textureSet;
}

RenderTargetTextureHandle TextureManager::CreateRenderTargetTexture(
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (!initialized_ || device_ == nullptr ||
        width == 0 || height == 0)
    {
        throw std::invalid_argument(
            "A render-target texture requires an initialized manager "
            "and a positive size.");
    }
    auto target = std::make_shared<D3D12RenderTargetTexture>();
    auto textureSet = std::make_shared<D3D12TextureSet>();
    const D3D12_CPU_DESCRIPTOR_HANDLE descriptorBlock =
        InitializeDescriptorBlock(*textureSet);

    D3D12_RESOURCE_DESC description = TextureDescription(width, height);
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12Detail::ThrowIfFailed(
        device_->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(target->resource.ReleaseAndGetAddressOf())),
        "Create render-target texture");

    D3D12_SHADER_RESOURCE_VIEW_DESC shaderView{};
    shaderView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    shaderView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shaderView.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shaderView.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        target->resource.Get(),
        &shaderView,
        descriptorBlock);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDescription.NumDescriptors = 1;
    D3D12Detail::ThrowIfFailed(
        device_->CreateDescriptorHeap(
            &rtvHeapDescription,
            IID_PPV_ARGS(target->rtvHeap.ReleaseAndGetAddressOf())),
        "Create render-target texture RTV heap");
    target->rtv =
        target->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(
        target->resource.Get(),
        nullptr,
        target->rtv);

    textureSet->Append({width, height}, target->resource);
    target->textures = textureSet;
    target->width = width;
    target->height = height;
    return target;
}

void TextureManager::ValidateTextureSetRequest(
    const std::span<const std::filesystem::path> paths) const
{
    if (!initialized_ || device_ == nullptr || commandQueue_ == nullptr)
    {
        throw std::logic_error(
            "TextureManager must be initialized before loading images.");
    }
    if (paths.empty() || paths.size() > MaxTexturesPerSet)
    {
        throw std::invalid_argument(
            "A texture set must contain between 1 and 64 images.");
    }
}

TextureManager::DecodedImage TextureManager::DecodeImage(
    const std::filesystem::path& path)
{
    ComPtr<IWICImagingFactory> factory;
    D3D12Detail::ThrowIfFailed(
        CoCreateInstance(
            CLSID_WICImagingFactory2,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
        "Create WIC imaging factory");

    ComPtr<IWICBitmapDecoder> decoder;
    D3D12Detail::ThrowIfFailed(
        factory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            decoder.ReleaseAndGetAddressOf()),
        "Open texture image");

    ComPtr<IWICBitmapFrameDecode> frame;
    D3D12Detail::ThrowIfFailed(
        decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()),
        "Read texture frame");

    UINT width = 0;
    UINT height = 0;
    D3D12Detail::ThrowIfFailed(frame->GetSize(&width, &height), "Read texture dimensions");
    if (width == 0 || height == 0 ||
        width > std::numeric_limits<UINT>::max() / 4)
    {
        throw std::runtime_error(
            "The texture dimensions are empty or too large.");
    }

    ComPtr<IWICFormatConverter> converter;
    D3D12Detail::ThrowIfFailed(
        factory->CreateFormatConverter(
            converter.ReleaseAndGetAddressOf()),
        "Create WIC format converter");
    D3D12Detail::ThrowIfFailed(
        converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom),
        "Convert texture to RGBA8");

    const UINT stride = width * 4;
    const std::uint64_t pixelByteCount =
        static_cast<std::uint64_t>(stride) * height;
    if (pixelByteCount > std::numeric_limits<UINT>::max() ||
        pixelByteCount > std::numeric_limits<std::size_t>::max())
    {
        throw std::runtime_error(
            "The decoded texture exceeds WIC buffer limits.");
    }

    DecodedImage image{};
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(pixelByteCount));
    D3D12Detail::ThrowIfFailed(
        converter->CopyPixels(
            nullptr,
            stride,
            static_cast<UINT>(pixelByteCount),
            reinterpret_cast<BYTE*>(image.pixels.data())),
        "Copy decoded texture pixels");
    return image;
}

void TextureManager::BeginUploadCommands()
{
    CollectCompletedUploads();
    if (!uploadAllocator_)
    {
        D3D12Detail::ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(uploadAllocator_.ReleaseAndGetAddressOf())),
                      "Create texture upload allocator");
        D3D12Detail::ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAllocator_.Get(), nullptr,
                                                 IID_PPV_ARGS(uploadCommandList_.ReleaseAndGetAddressOf())),
                      "Create texture upload list");
    }
    else
    {
        D3D12Detail::ThrowIfFailed(uploadAllocator_->Reset(), "Reset texture upload allocator");
        D3D12Detail::ThrowIfFailed(uploadCommandList_->Reset(uploadAllocator_.Get(), nullptr), "Reset texture upload list");
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE TextureManager::InitializeDescriptorBlock(
    TextureSet& textureSet)
{
    CollectCompletedUploads();
    D3D12TextureSet& concreteSet = RequireTextureSet(textureSet);
    const auto allocation = descriptorBlocks_->Acquire(descriptorBlocks_);
    concreteSet.descriptorAllocation = allocation;
    const D3D12_CPU_DESCRIPTOR_HANDLE cpuBlockStart =
        OffsetCpuDescriptor(
            descriptorHeap_->GetCPUDescriptorHandleForHeapStart(),
            allocation->offset,
            descriptorSize_);
    concreteSet.cpuDescriptorStart = cpuBlockStart;
    concreteSet.gpuDescriptorStart = OffsetGpuDescriptor(
        descriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
        allocation->offset,
        descriptorSize_);

    // Fill the fixed-size shader array with safe null SRVs first. Valid
    // images overwrite only the descriptors they use.
    D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};
    nullView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullView.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullView.Texture2D.MipLevels = 1;
    for (std::uint32_t index = 0; index < MaxTexturesPerSet; ++index)
    {
        device_->CreateShaderResourceView(
            nullptr,
            &nullView,
            OffsetCpuDescriptor(cpuBlockStart, index, descriptorSize_));
    }
    return cpuBlockStart;
}

void TextureManager::RecordTextureUpload(
    const DecodedImage& image,
    const std::size_t descriptorIndex,
    const D3D12_CPU_DESCRIPTOR_HANDLE descriptorBlockStart,
    TextureSet& textureSet,
    std::vector<ComPtr<ID3D12Resource>>& uploadBuffers)
{
    // Allocate the default-heap destination and query the exact upload
    // footprint required by this image's row alignment.
    const D3D12_RESOURCE_DESC textureDescription =
        TextureDescription(image.width, image.height);
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    D3D12Detail::ThrowIfFailed(
        device_->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDescription,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())),
        "Create GPU texture");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 sourceRowSize = 0;
    UINT64 uploadByteCount = 0;
    device_->GetCopyableFootprints(
        &textureDescription,
        0,
        1,
        0,
        &footprint,
        &rowCount,
        &sourceRowSize,
        &uploadByteCount);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC uploadDescription =
        BufferDescription(uploadByteCount);
    ComPtr<ID3D12Resource> uploadBuffer;
    D3D12Detail::ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(uploadBuffer.ReleaseAndGetAddressOf())),
        "Create texture upload buffer");

    // Copy tightly packed WIC RGBA rows into the D3D12-aligned upload
    // footprint without changing the source image dimensions.
    std::byte* mappedPixels = nullptr;
    D3D12_RANGE noReadRange{0, 0};
    D3D12Detail::ThrowIfFailed(
        uploadBuffer->Map(
            0,
            &noReadRange,
            reinterpret_cast<void**>(&mappedPixels)),
        "Map texture upload buffer");
    const std::size_t sourceStride =
        static_cast<std::size_t>(image.width) * 4;
    for (UINT row = 0; row < rowCount; ++row)
    {
        std::memcpy(
            mappedPixels + footprint.Offset +
                static_cast<std::size_t>(row) *
                    footprint.Footprint.RowPitch,
            image.pixels.data() +
                static_cast<std::size_t>(row) * sourceStride,
            sourceStride);
    }
    uploadBuffer->Unmap(0, nullptr);

    // Record the copy and transition so the resource is shader-readable
    // when the initialization upload fence completes.
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = uploadBuffer.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    uploadCommandList_->CopyTextureRegion(
        &destination,
        0,
        0,
        0,
        &source,
        nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uploadCommandList_->ResourceBarrier(1, &barrier);

    // The descriptor overwrites one null entry in this TextureSet's fixed
    // 64-slot SRV block; differently sized Texture2D resources stay
    // independent.
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        texture.Get(),
        &view,
        OffsetCpuDescriptor(
            descriptorBlockStart,
            static_cast<std::uint32_t>(descriptorIndex),
            descriptorSize_));

    RequireTextureSet(textureSet).Append(
        TextureInfo{image.width, image.height},
        std::move(texture));
    uploadBuffers.push_back(std::move(uploadBuffer));
}

void TextureManager::CollectCompletedUploads()
{
    if (!uploadFence_)
        return;
    while (!pendingUploads_.empty() && pendingUploads_.front().fence != 0 &&
           uploadFence_->GetCompletedValue() >= pendingUploads_.front().fence)
        pendingUploads_.pop_front();
}

void TextureManager::ExecuteUpload(std::vector<ComPtr<ID3D12Resource>> buffers, TextureSetHandle target)
{
    const auto fenceValue = nextUploadFenceValue_++;
    pendingUploads_.push_back(
        {fenceValue, uploadAllocator_, uploadCommandList_, std::move(buffers), std::move(target)});
    ID3D12CommandList *commands[]{uploadCommandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commands);
    const auto signalResult = commandQueue_->Signal(uploadFence_.Get(), fenceValue);
    // Later draws use this same queue, which orders them after the copy.
    // Keep the allocator, command list, upload and destination alive on CPU.
    uploadAllocator_.Reset();
    uploadCommandList_.Reset();
    if (FAILED(signalResult))
        pendingUploads_.back().fence = 0;
    D3D12Detail::ThrowIfFailed(signalResult, "Signal texture upload fence");
}

ID3D12DescriptorHeap* TextureManager::DescriptorHeap() const noexcept
{
    return descriptorHeap_.Get();
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GpuDescriptorStart(
    const TextureSetHandle& textureSet) const
{
    if (!initialized_ || textureSet == nullptr)
    {
        throw std::invalid_argument(
            "A GPU descriptor table requires an initialized manager and texture set.");
    }
    return RequireTextureSet(textureSet).gpuDescriptorStart;
}

void TextureManager::BeginRenderTargetPass(
    ID3D12GraphicsCommandList& commandList,
    const RenderTargetTextureHandle& target,
    const DirectX::XMFLOAT4& clearColor)
{
    if (!initialized_ || target == nullptr)
    {
        throw std::invalid_argument(
            "Beginning a texture pass requires an initialized manager and target.");
    }
    D3D12RenderTargetTexture& concreteTarget =
        RequireRenderTarget(target);

    if (concreteTarget.state != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = concreteTarget.resource.Get();
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = concreteTarget.state;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList.ResourceBarrier(1, &barrier);
        concreteTarget.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    const D3D12_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(concreteTarget.width),
        static_cast<float>(concreteTarget.height),
        0.0F,
        1.0F};
    const D3D12_RECT scissor{
        0,
        0,
        static_cast<LONG>(concreteTarget.width),
        static_cast<LONG>(concreteTarget.height)};
    commandList.RSSetViewports(1, &viewport);
    commandList.RSSetScissorRects(1, &scissor);
    commandList.OMSetRenderTargets(
        1,
        &concreteTarget.rtv,
        FALSE,
        nullptr);
    const float color[]{
        clearColor.x,
        clearColor.y,
        clearColor.z,
        clearColor.w};
    commandList.ClearRenderTargetView(
        concreteTarget.rtv, color, 0, nullptr);
}

void TextureManager::EndRenderTargetPass(
    ID3D12GraphicsCommandList& commandList,
    const RenderTargetTextureHandle& target)
{
    if (!initialized_ || target == nullptr)
    {
        throw std::invalid_argument(
            "Ending a texture pass requires an initialized manager and target.");
    }
    D3D12RenderTargetTexture& concreteTarget =
        RequireRenderTarget(target);
    if (concreteTarget.state ==
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = concreteTarget.resource.Get();
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = concreteTarget.state;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList.ResourceBarrier(1, &barrier);
    concreteTarget.state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
