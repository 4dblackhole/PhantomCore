#pragma once

#include "Query/Collision.h"
#include "Shape/Shape.h"
#include "Shape/Vertex.h"
#include "Texture/TextureManager.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>


inline constexpr std::uint32_t NoTextureIndex =
    std::numeric_limits<std::uint32_t>::max();

enum class MeshVertexLayout : std::uint8_t
{
    Unsupported,
    Position,
    PositionUv,
    PositionColor,
    PositionUvColor,
    PositionNormal,
    PositionNormalUv,
    PositionNormalUvColor,
};

template <typename VertexType>
inline constexpr MeshVertexLayout MeshVertexLayoutFor =
    MeshVertexLayout::Unsupported;

template <>
inline constexpr MeshVertexLayout
    MeshVertexLayoutFor<VertexPosition> =
        MeshVertexLayout::Position;
template <>
inline constexpr MeshVertexLayout
    MeshVertexLayoutFor<VertexPositionUv> =
        MeshVertexLayout::PositionUv;
template <>
inline constexpr MeshVertexLayout
    MeshVertexLayoutFor<VertexPositionColor> =
        MeshVertexLayout::PositionColor;
template <>
inline constexpr MeshVertexLayout
    MeshVertexLayoutFor<VertexPositionUvColor> =
        MeshVertexLayout::PositionUvColor;
template <>
inline constexpr MeshVertexLayout
    MeshVertexLayoutFor<VertexPositionNormal> =
        MeshVertexLayout::PositionNormal;
template <>
inline constexpr MeshVertexLayout
    MeshVertexLayoutFor<VertexPositionNormalUv> =
        MeshVertexLayout::PositionNormalUv;
template <>
inline constexpr MeshVertexLayout
    MeshVertexLayoutFor<VertexPositionNormalUvColor> =
        MeshVertexLayout::PositionNormalUvColor;

// Immutable GPU copy of one Shape.  Geometry remains CPU/backend-neutral;
// this object exclusively owns the D3D12 vertex and index resources.
class GpuMesh final
{
public:
    ~GpuMesh();

    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

    [[nodiscard]] MeshVertexLayout VertexLayout() const noexcept;
    [[nodiscard]] std::uint32_t IndexCount() const noexcept;
    [[nodiscard]] const Sphere3D&
        LocalBoundingSphere() const noexcept;

private:
    friend class MeshRenderSystem;

    GpuMesh();

    struct Impl;
    std::unique_ptr<Impl> implementation_;
    MeshVertexLayout vertexLayout_{MeshVertexLayout::Unsupported};
    std::uint32_t indexCount_{};
    Sphere3D localBoundingSphere_{};
};

using GpuMeshHandle = std::shared_ptr<const GpuMesh>;

enum class BuiltInMaterial : std::uint8_t
{
    UnlitVertexColor,
    UnlitVertexColorTextureArray,
    UnlitVertexColorTextureArrayAlphaBlend,
};

// Shared shader/root-signature/PSO definition.  Instances refer to this
// immutable template so expensive pipeline objects are created once.
class MaterialTemplate final
{
public:
    ~MaterialTemplate();

    MaterialTemplate(const MaterialTemplate&) = delete;
    MaterialTemplate& operator=(const MaterialTemplate&) = delete;

    [[nodiscard]] BuiltInMaterial Type() const noexcept;
    [[nodiscard]] MeshVertexLayout RequiredVertexLayout() const noexcept;

private:
    friend class MaterialInstance;
    friend class MeshRenderSystem;

    MaterialTemplate();

    BuiltInMaterial type_{BuiltInMaterial::UnlitVertexColor};
    MeshVertexLayout requiredVertexLayout_{
        MeshVertexLayout::PositionColor};
    bool usesTextureSet_{};
    bool alphaBlended_{};
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

using MaterialTemplateHandle = std::shared_ptr<const MaterialTemplate>;

// Per-material binding object.  A textured instance retains its
// TextureSet so the SRV descriptors and underlying resources remain valid
// across every in-flight frame.
class MaterialInstance final
{
public:
    MaterialInstance(const MaterialInstance&) = delete;
    MaterialInstance& operator=(const MaterialInstance&) = delete;

    [[nodiscard]] const MaterialTemplateHandle& Template() const noexcept;
    void SetTextureSet(TextureSetHandle textureSet);
    [[nodiscard]] const TextureSetHandle& Textures() const noexcept;

private:
    friend class MeshRenderSystem;

    explicit MaterialInstance(MaterialTemplateHandle materialTemplate);

    MaterialTemplateHandle materialTemplate_;
    TextureSetHandle textureSet_;
};

using MaterialInstanceHandle = std::shared_ptr<MaterialInstance>;

// Owns common material pipelines, uploads Shape data, batches submitted
// mesh instances, and records DrawIndexedInstanced calls.
class MeshRenderSystem final
{
public:
    static constexpr std::uint32_t FrameCount = 2;

    MeshRenderSystem() = default;
    ~MeshRenderSystem();

    MeshRenderSystem(const MeshRenderSystem&) = delete;
    MeshRenderSystem& operator=(const MeshRenderSystem&) = delete;

    void Initialize(
        ID3D12Device& device,
        ID3D12CommandQueue& commandQueue,
        DXGI_FORMAT renderTargetFormat,
        DXGI_FORMAT depthStencilFormat);
    void Shutdown() noexcept;

    template <ShapeVertex VertexType>
    [[nodiscard]] GpuMeshHandle CreateMesh(
        const Shape& shape)
    {
        constexpr MeshVertexLayout layout =
            MeshVertexLayoutFor<std::remove_cv_t<VertexType>>;
        static_assert(
            layout != MeshVertexLayout::Unsupported,
            "Register this vertex type with MeshVertexLayoutFor before "
            "uploading it.");

        const std::vector<VertexType> vertices =
            shape.CreateVertices<VertexType>();
        const std::span<const VertexType> vertexSpan{
            vertices.data(),
            vertices.size()};
        return CreateMeshData(
            std::as_bytes(vertexSpan),
            sizeof(VertexType),
            layout,
            shape.Indices(),
            ComputeLocalBoundingSphere(shape));
    }

    [[nodiscard]] MaterialInstanceHandle CreateMaterial(
        BuiltInMaterial material);
    [[nodiscard]] TextureManager& Textures() noexcept;
    [[nodiscard]] const TextureManager& Textures() const noexcept;
    [[nodiscard]] ID3D12Device* Device() const noexcept;

    // Called by D3D12Renderer after the selected frame resource is safe.
    void BeginFrame(std::uint32_t frameIndex);

    // Stores CPU-side instance data only.  Flush sorts equal mesh/material
    // pairs and records one instanced draw for each batch.
    void Submit(
        GpuMeshHandle mesh,
        MaterialInstanceHandle material,
        const DirectX::XMFLOAT4X4& world,
        const DirectX::XMFLOAT4X4& viewProjection,
        const DirectX::XMFLOAT4& color,
        const DirectX::XMFLOAT2& uvScale,
        const DirectX::XMFLOAT2& uvOffset,
        std::uint32_t textureIndex,
        DirectX::XMFLOAT4 clipRectPixels = {
            -1.0e9F, -1.0e9F, 1.0e9F, 1.0e9F},
        DirectX::XMFLOAT4 roundedRectLocal = {},
        float cornerRadiusLocal = 0.0F);

    void Flush(ID3D12GraphicsCommandList& commandList);

private:
    struct InstanceData
    {
        DirectX::XMFLOAT4X4 worldViewProjection{};
        DirectX::XMFLOAT4 color{1.0F, 1.0F, 1.0F, 1.0F};
        DirectX::XMFLOAT4 uvTransform{1.0F, 1.0F, 0.0F, 0.0F};
        DirectX::XMFLOAT4 clipRectPixels{
            -1.0e9F, -1.0e9F, 1.0e9F, 1.0e9F};
        DirectX::XMFLOAT4 roundedRectLocal{};
        float cornerRadiusLocal{};
        std::uint32_t textureIndex{NoTextureIndex};
        std::array<std::uint32_t, 2> padding{};
    };

    struct PendingItem
    {
        GpuMeshHandle mesh;
        MaterialInstanceHandle material;
        InstanceData instance;
    };

    struct FrameInstanceBuffer
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::byte* mappedData{};
        std::size_t capacity{};
        // A scene may be destroyed immediately after it submits a frame.
        // Keep the referenced GPU objects alive until this frame index is
        // selected again after D3D12Renderer's fence wait.
        std::vector<GpuMeshHandle> retainedMeshes;
        std::vector<MaterialInstanceHandle> retainedMaterials;
    };

    [[nodiscard]] GpuMeshHandle CreateMeshData(
        std::span<const std::byte> vertexBytes,
        std::size_t vertexStrideBytes,
        MeshVertexLayout layout,
        std::span<const std::uint32_t> indices,
        const Sphere3D& localBoundingSphere);
    [[nodiscard]] static Sphere3D ComputeLocalBoundingSphere(
        const Shape& shape) noexcept;
    [[nodiscard]] std::shared_ptr<MaterialTemplate>
        CreateUnlitVertexColorTemplate();
    [[nodiscard]] std::shared_ptr<MaterialTemplate>
        CreateUnlitVertexColorTextureArrayTemplate(
            BuiltInMaterial materialType,
            bool alphaBlended);
    void SortPendingItems();
    void UploadPendingInstances(FrameInstanceBuffer& instanceBuffer);
    [[nodiscard]] std::size_t FindBatchEnd(
        std::size_t batchStart) const noexcept;
    void DrawBatch(
        ID3D12GraphicsCommandList& commandList,
        const FrameInstanceBuffer& instanceBuffer,
        std::size_t batchStart,
        std::size_t batchEnd);
    void EnsureInstanceCapacity(
        std::uint32_t frameIndex,
        std::size_t requiredInstanceCount);

    ID3D12Device* device_{};
    DXGI_FORMAT renderTargetFormat_{DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_FORMAT depthStencilFormat_{DXGI_FORMAT_D32_FLOAT};
    std::shared_ptr<MaterialTemplate> unlitVertexColorTemplate_;
    std::shared_ptr<MaterialTemplate>
        unlitVertexColorTextureArrayTemplate_;
    std::shared_ptr<MaterialTemplate>
        unlitVertexColorTextureArrayAlphaBlendTemplate_;
    TextureManager textureManager_;
    std::array<FrameInstanceBuffer, FrameCount> instanceBuffers_{};
    std::vector<PendingItem> pendingItems_;
    std::uint32_t currentFrameIndex_{};
    bool initialized_{};
    bool frameOpen_{};
};
