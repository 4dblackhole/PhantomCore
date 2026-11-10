#include "Mesh/MeshRendering.h"
#include "Renderer/D3D12Error.h"

#include "Shader/ShaderCompiler.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>

using namespace DirectX;
using Microsoft::WRL::ComPtr;


namespace
{
    [[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(
        const std::size_t byteCount) noexcept
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = byteCount;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return description;
    }

    [[nodiscard]] ComPtr<ID3D12Resource> CreateUploadBuffer(
        ID3D12Device& device,
        const std::size_t byteCount)
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC description =
            BufferDescription(byteCount);

        ComPtr<ID3D12Resource> resource;
        D3D12Detail::ThrowIfFailed(
            device.CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())),
            "Create upload buffer");
        return resource;
    }

    [[nodiscard]] D3D12_RENDER_TARGET_BLEND_DESC
        OpaqueBlendDescription() noexcept
    {
        D3D12_RENDER_TARGET_BLEND_DESC description{};
        description.BlendEnable = FALSE;
        description.LogicOpEnable = FALSE;
        description.SrcBlend = D3D12_BLEND_ONE;
        description.DestBlend = D3D12_BLEND_ZERO;
        description.BlendOp = D3D12_BLEND_OP_ADD;
        description.SrcBlendAlpha = D3D12_BLEND_ONE;
        description.DestBlendAlpha = D3D12_BLEND_ZERO;
        description.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        description.LogicOp = D3D12_LOGIC_OP_NOOP;
        description.RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        return description;
    }

    [[nodiscard]] D3D12_RENDER_TARGET_BLEND_DESC
        AlphaBlendDescription() noexcept
    {
        D3D12_RENDER_TARGET_BLEND_DESC description{};
        description.BlendEnable = TRUE;
        description.LogicOpEnable = FALSE;
        description.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        description.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        description.BlendOp = D3D12_BLEND_OP_ADD;
        description.SrcBlendAlpha = D3D12_BLEND_ONE;
        description.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        description.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        description.LogicOp = D3D12_LOGIC_OP_NOOP;
        description.RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        return description;
    }

    [[nodiscard]] std::size_t NextCapacity(
        const std::size_t requiredCount)
    {
        std::size_t capacity = 256;
        while (capacity < requiredCount)
        {
            if (capacity > std::numeric_limits<std::size_t>::max() / 2)
            {
                throw std::overflow_error(
                    "The mesh instance buffer capacity overflowed.");
            }
            capacity *= 2;
        }
        return capacity;
    }
}

MeshVertexLayout GpuMesh::VertexLayout() const noexcept
{
    return vertexLayout_;
}

std::uint32_t GpuMesh::IndexCount() const noexcept
{
    return indexCount_;
}

const Sphere3D&
GpuMesh::LocalBoundingSphere() const noexcept
{
    return localBoundingSphere_;
}

BuiltInMaterial MaterialTemplate::Type() const noexcept
{
    return type_;
}

MeshVertexLayout MaterialTemplate::RequiredVertexLayout() const noexcept
{
    return requiredVertexLayout_;
}

MaterialInstance::MaterialInstance(
    MaterialTemplateHandle materialTemplate)
    : materialTemplate_(std::move(materialTemplate))
{
}

const MaterialTemplateHandle& MaterialInstance::Template() const noexcept
{
    return materialTemplate_;
}

void MaterialInstance::SetTextureSet(TextureSetHandle textureSet)
{
    if (textureSet == nullptr)
    {
        throw std::invalid_argument(
            "A material texture set cannot be null.");
    }
    if (materialTemplate_ == nullptr ||
        !materialTemplate_->usesTextureSet_)
    {
        throw std::logic_error(
            "This material template does not expose texture slots.");
    }
    textureSet_ = std::move(textureSet);
}

const TextureSetHandle& MaterialInstance::Textures() const noexcept
{
    return textureSet_;
}

MeshRenderSystem::~MeshRenderSystem()
{
    Shutdown();
}

void MeshRenderSystem::Initialize(
    ID3D12Device& device,
    ID3D12CommandQueue& commandQueue,
    const DXGI_FORMAT renderTargetFormat,
    const DXGI_FORMAT depthStencilFormat)
{
    if (initialized_)
    {
        throw std::logic_error(
            "MeshRenderSystem is already initialized.");
    }

    device_ = &device;
    renderTargetFormat_ = renderTargetFormat;
    depthStencilFormat_ = depthStencilFormat;

    try
    {
        textureManager_.Initialize(device, commandQueue);

        // MaterialTemplate creation compiles the common engine shader and
        // owns its root signature/PSO.  Client scenes never create them.
        unlitVertexColorTemplate_ =
            CreateUnlitVertexColorTemplate();
        unlitVertexColorTextureArrayTemplate_ =
            CreateUnlitVertexColorTextureArrayTemplate(
                BuiltInMaterial::UnlitVertexColorTextureArray,
                false);
        unlitVertexColorTextureArrayAlphaBlendTemplate_ =
            CreateUnlitVertexColorTextureArrayTemplate(
                BuiltInMaterial::
                    UnlitVertexColorTextureArrayAlphaBlend,
                true);
        initialized_ = true;
    }
    catch (...)
    {
        Shutdown();
        throw;
    }
}

void MeshRenderSystem::Shutdown() noexcept
{
    pendingItems_.clear();

    for (FrameInstanceBuffer& buffer : instanceBuffers_)
    {
        buffer.retainedMaterials.clear();
        buffer.retainedMeshes.clear();
        if (buffer.resource != nullptr && buffer.mappedData != nullptr)
        {
            buffer.resource->Unmap(0, nullptr);
        }
        buffer.mappedData = nullptr;
        buffer.capacity = 0;
        buffer.resource.Reset();
    }

    unlitVertexColorTextureArrayAlphaBlendTemplate_.reset();
    unlitVertexColorTextureArrayTemplate_.reset();
    unlitVertexColorTemplate_.reset();
    textureManager_.Shutdown();
    device_ = nullptr;
    initialized_ = false;
    frameOpen_ = false;
    currentFrameIndex_ = 0;
}

MaterialInstanceHandle MeshRenderSystem::CreateMaterial(
    const BuiltInMaterial material)
{
    if (!initialized_)
    {
        throw std::logic_error(
            "MeshRenderSystem must be initialized before creating a "
            "material.");
    }

    MaterialTemplateHandle materialTemplate;
    switch (material)
    {
    case BuiltInMaterial::UnlitVertexColor:
        materialTemplate = unlitVertexColorTemplate_;
        break;
    case BuiltInMaterial::UnlitVertexColorTextureArray:
        materialTemplate = unlitVertexColorTextureArrayTemplate_;
        break;
    case BuiltInMaterial::UnlitVertexColorTextureArrayAlphaBlend:
        materialTemplate =
            unlitVertexColorTextureArrayAlphaBlendTemplate_;
        break;
    }

    if (materialTemplate == nullptr)
    {
        throw std::runtime_error(
            "The requested built-in material is unavailable.");
    }

    return MaterialInstanceHandle{
        new MaterialInstance(std::move(materialTemplate))};
}

TextureManager& MeshRenderSystem::Textures() noexcept
{
    return textureManager_;
}

const TextureManager& MeshRenderSystem::Textures() const noexcept
{
    return textureManager_;
}

ID3D12Device* MeshRenderSystem::Device() const noexcept
{
    return device_;
}

void MeshRenderSystem::BeginFrame(const std::uint32_t frameIndex)
{
    if (!initialized_ || frameIndex >= FrameCount)
    {
        throw std::logic_error(
            "MeshRenderSystem received an invalid frame.");
    }
    if (frameOpen_)
    {
        throw std::logic_error(
            "A MeshRenderSystem frame is already open.");
    }

    currentFrameIndex_ = frameIndex;
    textureManager_.CollectCompletedUploads();
    // D3D12Renderer waited for this frame index before calling us, so GPU
    // resources retained by its previous submission are now releasable.
    instanceBuffers_[currentFrameIndex_].retainedMaterials.clear();
    instanceBuffers_[currentFrameIndex_].retainedMeshes.clear();
    pendingItems_.clear();
    frameOpen_ = true;
}

void MeshRenderSystem::Submit(
    GpuMeshHandle mesh,
    MaterialInstanceHandle material,
    const XMFLOAT4X4& world,
    const XMFLOAT4X4& viewProjection,
    const XMFLOAT4& color,
    const XMFLOAT2& uvScale,
    const XMFLOAT2& uvOffset,
    const std::uint32_t textureIndex,
    const XMFLOAT4 clipRectPixels,
    const XMFLOAT4 roundedRectLocal,
    const float cornerRadiusLocal)
{
    if (!frameOpen_)
    {
        throw std::logic_error(
            "Mesh instances can only be submitted during an open frame.");
    }
    if (mesh == nullptr || material == nullptr ||
        material->Template() == nullptr)
    {
        throw std::invalid_argument(
            "A mesh submission requires a mesh and material.");
    }
    if (mesh->VertexLayout() !=
        material->Template()->RequiredVertexLayout())
    {
        throw std::invalid_argument(
            "The mesh vertex layout does not match its material.");
    }
    if (material->Template()->usesTextureSet_)
    {
        if (material->Textures() == nullptr)
        {
            throw std::invalid_argument(
                "A textured material requires a texture set.");
        }
        if (textureIndex != NoTextureIndex &&
            textureIndex >= material->Textures()->Size())
        {
            throw std::out_of_range(
                "The mesh texture index is outside its material set.");
        }
    }
    else if (textureIndex != NoTextureIndex)
    {
        throw std::invalid_argument(
            "A texture index was submitted to an untextured material.");
    }

    InstanceData instance{};
    XMStoreFloat4x4(
        &instance.worldViewProjection,
        XMLoadFloat4x4(&world) *
            XMLoadFloat4x4(&viewProjection));
    instance.color = color;
    instance.uvTransform = {
        uvScale.x,
        uvScale.y,
        uvOffset.x,
        uvOffset.y};
    instance.clipRectPixels = clipRectPixels;
    instance.roundedRectLocal = roundedRectLocal;
    instance.cornerRadiusLocal = cornerRadiusLocal;
    instance.textureIndex = textureIndex;

    pendingItems_.push_back(PendingItem{
        std::move(mesh),
        std::move(material),
        instance});
}

void MeshRenderSystem::Flush(ID3D12GraphicsCommandList& commandList)
{
    if (!frameOpen_)
    {
        throw std::logic_error(
            "No MeshRenderSystem frame is open.");
    }

    if (pendingItems_.empty())
    {
        frameOpen_ = false;
        return;
    }

    SortPendingItems();
    EnsureInstanceCapacity(
        currentFrameIndex_,
        pendingItems_.size());
    FrameInstanceBuffer& instanceBuffer =
        instanceBuffers_[currentFrameIndex_];
    UploadPendingInstances(instanceBuffer);

    // The descriptor heap is shared by every TextureSet.  Materials bind
    // only their own fixed-size block before drawing.
    ID3D12DescriptorHeap* descriptorHeaps[]{
        textureManager_.DescriptorHeap()};
    commandList.SetDescriptorHeaps(1, descriptorHeaps);

    std::size_t batchStart = 0;
    while (batchStart < pendingItems_.size())
    {
        const std::size_t batchEnd = FindBatchEnd(batchStart);
        DrawBatch(commandList, instanceBuffer, batchStart, batchEnd);
        batchStart = batchEnd;
    }

    // Transfer the submission's shared ownership to this frame resource.
    // MaterialInstance also retains its TextureSet and descriptors.
    instanceBuffer.retainedMeshes.reserve(pendingItems_.size());
    instanceBuffer.retainedMaterials.reserve(pendingItems_.size());
    for (PendingItem& item : pendingItems_)
    {
        instanceBuffer.retainedMeshes.push_back(std::move(item.mesh));
        instanceBuffer.retainedMaterials.push_back(
            std::move(item.material));
    }
    pendingItems_.clear();
    frameOpen_ = false;
}

void MeshRenderSystem::SortPendingItems()
{
    // Opaque draws can be freely regrouped by material and mesh because
    // the depth buffer resolves their visibility. Alpha-blended draws run
    // afterwards in their original submission order so Canvas painter
    // order is preserved while adjacent compatible images still batch.
    const std::less<const void*> pointerLess;
    std::stable_sort(
        pendingItems_.begin(),
        pendingItems_.end(),
        [&pointerLess](
            const PendingItem& left,
            const PendingItem& right)
        {
            const bool leftAlpha =
                left.material->Template()->alphaBlended_;
            const bool rightAlpha =
                right.material->Template()->alphaBlended_;
            if (leftAlpha != rightAlpha)
            {
                return !leftAlpha;
            }
            if (leftAlpha)
            {
                return false;
            }

            // A material instance is part of the opaque key because it
            // also identifies the TextureSet descriptor block.
            const void* leftMaterial = left.material.get();
            const void* rightMaterial = right.material.get();
            if (leftMaterial != rightMaterial)
            {
                return pointerLess(leftMaterial, rightMaterial);
            }
            return pointerLess(left.mesh.get(), right.mesh.get());
        });
}

void MeshRenderSystem::UploadPendingInstances(
    FrameInstanceBuffer& instanceBuffer)
{
    for (std::size_t index = 0; index < pendingItems_.size(); ++index)
    {
        std::memcpy(
            instanceBuffer.mappedData + index * sizeof(InstanceData),
            &pendingItems_[index].instance,
            sizeof(InstanceData));
    }
}

std::size_t MeshRenderSystem::FindBatchEnd(
    const std::size_t batchStart) const noexcept
{
    const PendingItem& first = pendingItems_[batchStart];
    std::size_t batchEnd = batchStart + 1;
    while (batchEnd < pendingItems_.size() &&
        pendingItems_[batchEnd].mesh.get() == first.mesh.get() &&
        pendingItems_[batchEnd].material.get() == first.material.get())
    {
        ++batchEnd;
    }
    return batchEnd;
}

void MeshRenderSystem::DrawBatch(
    ID3D12GraphicsCommandList& commandList,
    const FrameInstanceBuffer& instanceBuffer,
    const std::size_t batchStart,
    const std::size_t batchEnd)
{
    const std::size_t batchCount = batchEnd - batchStart;
    if (batchCount > std::numeric_limits<UINT>::max())
    {
        throw std::overflow_error(
            "A mesh instance batch exceeds the D3D12 draw limit.");
    }

    const PendingItem& first = pendingItems_[batchStart];
    const MaterialTemplateHandle& materialTemplate =
        first.material->Template();
    commandList.SetPipelineState(materialTemplate->pipelineState_.Get());
    commandList.SetGraphicsRootSignature(
        materialTemplate->rootSignature_.Get());
    if (materialTemplate->usesTextureSet_)
    {
        commandList.SetGraphicsRootDescriptorTable(
            0,
            textureManager_.GpuDescriptorStart(
                first.material->Textures()));
    }
    commandList.IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW instanceView{};
    instanceView.BufferLocation =
        instanceBuffer.resource->GetGPUVirtualAddress() +
        batchStart * sizeof(InstanceData);
    instanceView.SizeInBytes = static_cast<UINT>(
        batchCount * sizeof(InstanceData));
    instanceView.StrideInBytes = sizeof(InstanceData);

    const std::array vertexViews{
        first.mesh->vertexBufferView_,
        instanceView};
    commandList.IASetVertexBuffers(
        0,
        static_cast<UINT>(vertexViews.size()),
        vertexViews.data());
    commandList.IASetIndexBuffer(&first.mesh->indexBufferView_);
    commandList.DrawIndexedInstanced(
        first.mesh->indexCount_,
        static_cast<UINT>(batchCount),
        0,
        0,
        0);
}

GpuMeshHandle MeshRenderSystem::CreateMeshData(
    const std::span<const std::byte> vertexBytes,
    const std::size_t vertexStrideBytes,
    const MeshVertexLayout layout,
    const std::span<const std::uint32_t> indices,
    const Sphere3D& localBoundingSphere)
{
    if (!initialized_ || device_ == nullptr)
    {
        throw std::logic_error(
            "MeshRenderSystem must be initialized before uploading a "
            "mesh.");
    }
    if (vertexBytes.empty() || vertexStrideBytes == 0 ||
        indices.empty() ||
        vertexBytes.size() > std::numeric_limits<UINT>::max() ||
        indices.size_bytes() > std::numeric_limits<UINT>::max() ||
        indices.size() > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::invalid_argument(
            "The mesh data is empty or exceeds D3D12 buffer limits.");
    }

    std::shared_ptr<GpuMesh> mesh{new GpuMesh()};
    mesh->vertexLayout_ = layout;
    mesh->indexCount_ =
        static_cast<std::uint32_t>(indices.size());
    mesh->localBoundingSphere_ = localBoundingSphere;

    mesh->vertexBuffer_ =
        CreateUploadBuffer(*device_, vertexBytes.size());
    D3D12_RANGE noReadRange{0, 0};
    void* mappedVertices = nullptr;
    D3D12Detail::ThrowIfFailed(
        mesh->vertexBuffer_->Map(
            0,
            &noReadRange,
            &mappedVertices),
        "Map mesh vertex buffer");
    std::memcpy(
        mappedVertices,
        vertexBytes.data(),
        vertexBytes.size());
    mesh->vertexBuffer_->Unmap(0, nullptr);

    mesh->vertexBufferView_.BufferLocation =
        mesh->vertexBuffer_->GetGPUVirtualAddress();
    mesh->vertexBufferView_.SizeInBytes =
        static_cast<UINT>(vertexBytes.size());
    mesh->vertexBufferView_.StrideInBytes =
        static_cast<UINT>(vertexStrideBytes);

    mesh->indexBuffer_ =
        CreateUploadBuffer(*device_, indices.size_bytes());
    void* mappedIndices = nullptr;
    D3D12Detail::ThrowIfFailed(
        mesh->indexBuffer_->Map(
            0,
            &noReadRange,
            &mappedIndices),
        "Map mesh index buffer");
    std::memcpy(
        mappedIndices,
        indices.data(),
        indices.size_bytes());
    mesh->indexBuffer_->Unmap(0, nullptr);

    mesh->indexBufferView_.BufferLocation =
        mesh->indexBuffer_->GetGPUVirtualAddress();
    mesh->indexBufferView_.SizeInBytes =
        static_cast<UINT>(indices.size_bytes());
    mesh->indexBufferView_.Format = DXGI_FORMAT_R32_UINT;
    return mesh;
}

Sphere3D MeshRenderSystem::ComputeLocalBoundingSphere(
    const Shape& shape) noexcept
{
    const std::span<const VertexAttributes> vertices =
        shape.Vertices();
    if (vertices.empty())
    {
        return {{}, -1.0F};
    }

    XMFLOAT3 minimum = vertices.front().position;
    XMFLOAT3 maximum = minimum;
    for (const VertexAttributes& vertex : vertices)
    {
        minimum.x = std::min(minimum.x, vertex.position.x);
        minimum.y = std::min(minimum.y, vertex.position.y);
        minimum.z = std::min(minimum.z, vertex.position.z);
        maximum.x = std::max(maximum.x, vertex.position.x);
        maximum.y = std::max(maximum.y, vertex.position.y);
        maximum.z = std::max(maximum.z, vertex.position.z);
    }

    const XMFLOAT3 center{
        (minimum.x + maximum.x) * 0.5F,
        (minimum.y + maximum.y) * 0.5F,
        (minimum.z + maximum.z) * 0.5F};
    float radiusSquared = 0.0F;
    for (const VertexAttributes& vertex : vertices)
    {
        const float x = vertex.position.x - center.x;
        const float y = vertex.position.y - center.y;
        const float z = vertex.position.z - center.z;
        radiusSquared = std::max(
            radiusSquared,
            x * x + y * y + z * z);
    }
    return {center, std::sqrt(radiusSquared)};
}

std::shared_ptr<MaterialTemplate>
    MeshRenderSystem::CreateUnlitVertexColorTemplate()
{
    // Compile the embedded shader pair before creating any persistent
    // pipeline objects, so a shader error leaves no partial material.
    const ComPtr<ID3DBlob> vertexShader =
        CompileShader(
            BuiltInShader::UnlitVertexColorInstanced,
            "VSMain",
            "vs_5_1");
    const ComPtr<ID3DBlob> pixelShader =
        CompileShader(
            BuiltInShader::UnlitVertexColorInstanced,
            "PSMain",
            "ps_5_1");

    // This material has no shader resources, so its root signature only
    // enables the input assembler.
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> serializationErrors;
    const HRESULT serializationResult =
        D3D12SerializeRootSignature(
            &rootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSignature.ReleaseAndGetAddressOf(),
            serializationErrors.ReleaseAndGetAddressOf());
    if (FAILED(serializationResult))
    {
        const std::string details =
            serializationErrors != nullptr
                ? std::string(
                    static_cast<const char*>(
                        serializationErrors->GetBufferPointer()),
                    serializationErrors->GetBufferSize())
                : "No serialization diagnostics were returned.";
        throw std::runtime_error(
            "Root signature serialization failed: " + details);
    }

    std::shared_ptr<MaterialTemplate> material{
        new MaterialTemplate()};
    material->type_ = BuiltInMaterial::UnlitVertexColor;
    material->requiredVertexLayout_ =
        MeshVertexLayout::PositionColor;

    D3D12Detail::ThrowIfFailed(
        device_->CreateRootSignature(
            0,
            serializedRootSignature->GetBufferPointer(),
            serializedRootSignature->GetBufferSize(),
            IID_PPV_ARGS(
                material->rootSignature_.ReleaseAndGetAddressOf())),
        "Create unlit vertex-color root signature");

    // Slot 0 is per-vertex data. Slot 1 carries the WVP matrix and color
    // once per instance for DrawIndexedInstanced.
    const std::array inputLayout{
        D3D12_INPUT_ELEMENT_DESC{
            "POSITION",
            0,
            DXGI_FORMAT_R32G32B32_FLOAT,
            0,
            static_cast<UINT>(
                offsetof(VertexPositionColor, position)),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            0},
        D3D12_INPUT_ELEMENT_DESC{
            "COLOR",
            0,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            0,
            static_cast<UINT>(
                offsetof(VertexPositionColor, color)),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            0},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_WVP",
            0,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            1,
            static_cast<UINT>(
                offsetof(InstanceData, worldViewProjection) + 0),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
            1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_WVP",
            1,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            1,
            static_cast<UINT>(
                offsetof(InstanceData, worldViewProjection) + 16),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
            1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_WVP",
            2,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            1,
            static_cast<UINT>(
                offsetof(InstanceData, worldViewProjection) + 32),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
            1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_WVP",
            3,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            1,
            static_cast<UINT>(
                offsetof(InstanceData, worldViewProjection) + 48),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
            1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_COLOR",
            0,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            1,
            static_cast<UINT>(offsetof(InstanceData, color)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
            1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_CLIP_RECT", 0,
            DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(InstanceData, clipRectPixels)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_ROUNDED_RECT", 0,
            DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(InstanceData, roundedRectLocal)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_CORNER_RADIUS", 0,
            DXGI_FORMAT_R32_FLOAT, 1,
            static_cast<UINT>(offsetof(InstanceData, cornerRadiusLocal)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};

    // The remaining state is the shared opaque, depth-tested triangle
    // pipeline used by every vertex-color material instance.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature =
        material->rootSignature_.Get();
    pipelineDescription.VS = {
        vertexShader->GetBufferPointer(),
        vertexShader->GetBufferSize()};
    pipelineDescription.PS = {
        pixelShader->GetBufferPointer(),
        pixelShader->GetBufferSize()};
    pipelineDescription.BlendState.AlphaToCoverageEnable = FALSE;
    pipelineDescription.BlendState.IndependentBlendEnable = FALSE;
    for (auto& target :
        pipelineDescription.BlendState.RenderTarget)
    {
        target = OpaqueBlendDescription();
    }
    pipelineDescription.SampleMask =
        std::numeric_limits<UINT>::max();
    pipelineDescription.RasterizerState.FillMode =
        D3D12_FILL_MODE_SOLID;
    pipelineDescription.RasterizerState.CullMode =
        D3D12_CULL_MODE_NONE;
    pipelineDescription.RasterizerState.FrontCounterClockwise =
        FALSE;
    pipelineDescription.RasterizerState.DepthBias =
        D3D12_DEFAULT_DEPTH_BIAS;
    pipelineDescription.RasterizerState.DepthBiasClamp =
        D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pipelineDescription.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pipelineDescription.RasterizerState.DepthClipEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthWriteMask =
        D3D12_DEPTH_WRITE_MASK_ALL;
    pipelineDescription.DepthStencilState.DepthFunc =
        D3D12_COMPARISON_FUNC_LESS;
    pipelineDescription.DepthStencilState.StencilEnable = FALSE;
    pipelineDescription.InputLayout = {
        inputLayout.data(),
        static_cast<UINT>(inputLayout.size())};
    pipelineDescription.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] = renderTargetFormat_;
    pipelineDescription.DSVFormat = depthStencilFormat_;
    pipelineDescription.SampleDesc.Count = 1;

    D3D12Detail::ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &pipelineDescription,
            IID_PPV_ARGS(
                material->pipelineState_.ReleaseAndGetAddressOf())),
        "Create unlit vertex-color pipeline state");
    return material;
}

std::shared_ptr<MaterialTemplate>
    MeshRenderSystem::CreateUnlitVertexColorTextureArrayTemplate(
        const BuiltInMaterial materialType,
        const bool alphaBlended)
{
    // The textured variant uses its own embedded shader pair and one SRV
    // descriptor table containing independently sized Texture2D objects.
    const ComPtr<ID3DBlob> vertexShader =
        CompileShader(
            BuiltInShader::
                UnlitVertexColorTextureArrayInstanced,
            "VSMain",
            "vs_5_1");
    const ComPtr<ID3DBlob> pixelShader =
        CompileShader(
            BuiltInShader::
                UnlitVertexColorTextureArrayInstanced,
            "PSMain",
            "ps_5_1");

    // Root parameter 0 is the fixed TextureSet descriptor block. A static
    // clamp sampler makes centered cover UVs crop without sampling a
    // neighboring image.
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = MaxTexturesPerSet;
    textureRange.BaseShaderRegister = 0;
    textureRange.RegisterSpace = 0;
    textureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameter.DescriptorTable.NumDescriptorRanges = 1;
    rootParameter.DescriptorTable.pDescriptorRanges = &textureRange;
    rootParameter.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor =
        D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0F;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 1;
    rootDescription.pParameters = &rootParameter;
    rootDescription.NumStaticSamplers = 1;
    rootDescription.pStaticSamplers = &sampler;
    rootDescription.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> serializationErrors;
    const HRESULT serializationResult =
        D3D12SerializeRootSignature(
            &rootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSignature.ReleaseAndGetAddressOf(),
            serializationErrors.ReleaseAndGetAddressOf());
    if (FAILED(serializationResult))
    {
        const std::string details =
            serializationErrors != nullptr
                ? std::string(
                    static_cast<const char*>(
                        serializationErrors->GetBufferPointer()),
                    serializationErrors->GetBufferSize())
                : "No serialization diagnostics were returned.";
        throw std::runtime_error(
            "Textured root signature serialization failed: " +
            details);
    }

    std::shared_ptr<MaterialTemplate> material{
        new MaterialTemplate()};
    material->type_ = materialType;
    material->requiredVertexLayout_ =
        MeshVertexLayout::PositionUvColor;
    material->usesTextureSet_ = true;
    material->alphaBlended_ = alphaBlended;

    D3D12Detail::ThrowIfFailed(
        device_->CreateRootSignature(
            0,
            serializedRootSignature->GetBufferPointer(),
            serializedRootSignature->GetBufferSize(),
            IID_PPV_ARGS(
                material->rootSignature_.ReleaseAndGetAddressOf())),
        "Create textured vertex-color root signature");

    // In addition to WVP/color, each instance selects a descriptor and
    // supplies its centered cover UV scale/offset.
    const std::array inputLayout{
        D3D12_INPUT_ELEMENT_DESC{
            "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
            static_cast<UINT>(offsetof(
                VertexPositionUvColor,
                position)),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        D3D12_INPUT_ELEMENT_DESC{
            "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
            static_cast<UINT>(offsetof(
                VertexPositionUvColor,
                uv)),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        D3D12_INPUT_ELEMENT_DESC{
            "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
            static_cast<UINT>(offsetof(
                VertexPositionUvColor,
                color)),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_WVP", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(
                InstanceData,
                worldViewProjection) + 0),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_WVP", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(
                InstanceData,
                worldViewProjection) + 16),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_WVP", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(
                InstanceData,
                worldViewProjection) + 32),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_WVP", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(
                InstanceData,
                worldViewProjection) + 48),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_COLOR", 0,
            DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(InstanceData, color)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_UV_TRANSFORM", 0,
            DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(InstanceData, uvTransform)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_CLIP_RECT", 0,
            DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(InstanceData, clipRectPixels)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_ROUNDED_RECT", 0,
            DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
            static_cast<UINT>(offsetof(InstanceData, roundedRectLocal)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_CORNER_RADIUS", 0,
            DXGI_FORMAT_R32_FLOAT, 1,
            static_cast<UINT>(offsetof(InstanceData, cornerRadiusLocal)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        D3D12_INPUT_ELEMENT_DESC{
            "INSTANCE_TEXTURE_INDEX", 0,
            DXGI_FORMAT_R32_UINT, 1,
            static_cast<UINT>(offsetof(InstanceData, textureIndex)),
            D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};

    // Build the depth-tested PSO after both the root signature and
    // complete vertex/instance layout are fixed. Transparent materials
    // preserve the existing depth and blend over the opaque pass.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature =
        material->rootSignature_.Get();
    pipelineDescription.VS = {
        vertexShader->GetBufferPointer(),
        vertexShader->GetBufferSize()};
    pipelineDescription.PS = {
        pixelShader->GetBufferPointer(),
        pixelShader->GetBufferSize()};
    pipelineDescription.BlendState.AlphaToCoverageEnable = FALSE;
    pipelineDescription.BlendState.IndependentBlendEnable = FALSE;
    for (auto& target :
        pipelineDescription.BlendState.RenderTarget)
    {
        target = alphaBlended
            ? AlphaBlendDescription()
            : OpaqueBlendDescription();
    }
    pipelineDescription.SampleMask =
        std::numeric_limits<UINT>::max();
    pipelineDescription.RasterizerState.FillMode =
        D3D12_FILL_MODE_SOLID;
    pipelineDescription.RasterizerState.CullMode =
        D3D12_CULL_MODE_NONE;
    pipelineDescription.RasterizerState.FrontCounterClockwise = FALSE;
    pipelineDescription.RasterizerState.DepthBias =
        D3D12_DEFAULT_DEPTH_BIAS;
    pipelineDescription.RasterizerState.DepthBiasClamp =
        D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pipelineDescription.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pipelineDescription.RasterizerState.DepthClipEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthWriteMask =
        alphaBlended
            ? D3D12_DEPTH_WRITE_MASK_ZERO
            : D3D12_DEPTH_WRITE_MASK_ALL;
    pipelineDescription.DepthStencilState.DepthFunc =
        alphaBlended
            ? D3D12_COMPARISON_FUNC_LESS_EQUAL
            : D3D12_COMPARISON_FUNC_LESS;
    pipelineDescription.DepthStencilState.StencilEnable = FALSE;
    pipelineDescription.InputLayout = {
        inputLayout.data(),
        static_cast<UINT>(inputLayout.size())};
    pipelineDescription.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] = renderTargetFormat_;
    pipelineDescription.DSVFormat = depthStencilFormat_;
    pipelineDescription.SampleDesc.Count = 1;

    D3D12Detail::ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &pipelineDescription,
            IID_PPV_ARGS(
                material->pipelineState_.ReleaseAndGetAddressOf())),
        "Create textured vertex-color pipeline state");
    return material;
}

void MeshRenderSystem::EnsureInstanceCapacity(
    const std::uint32_t frameIndex,
    const std::size_t requiredInstanceCount)
{
    FrameInstanceBuffer& buffer = instanceBuffers_[frameIndex];
    if (buffer.capacity >= requiredInstanceCount)
    {
        return;
    }

    if (buffer.resource != nullptr && buffer.mappedData != nullptr)
    {
        buffer.resource->Unmap(0, nullptr);
    }
    buffer.mappedData = nullptr;
    buffer.resource.Reset();

    buffer.capacity = NextCapacity(requiredInstanceCount);
    if (buffer.capacity >
        std::numeric_limits<UINT>::max() / sizeof(InstanceData))
    {
        throw std::overflow_error(
            "The mesh instance buffer exceeds D3D12 limits.");
    }

    buffer.resource = CreateUploadBuffer(
        *device_,
        buffer.capacity * sizeof(InstanceData));
    D3D12_RANGE noReadRange{0, 0};
    D3D12Detail::ThrowIfFailed(
        buffer.resource->Map(
            0,
            &noReadRange,
            reinterpret_cast<void**>(&buffer.mappedData)),
        "Map mesh instance buffer");
}
