#pragma once

#include "Query/Collision.h"
#include "Shape/Shape.h"
#include "Shape/Vertex.h"
#include "Texture/TextureManager.h"

#include <DirectXMath.h>

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
    MeshRenderSystem();
    ~MeshRenderSystem();

    MeshRenderSystem(const MeshRenderSystem&) = delete;
    MeshRenderSystem& operator=(const MeshRenderSystem&) = delete;

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

    // Stores CPU-side instance data only. The engine batches equal
    // mesh/material pairs when it records the frame.
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

private:
    friend struct MeshRenderingBackend;
    struct Impl;
    std::unique_ptr<Impl> implementation_;

    [[nodiscard]] GpuMeshHandle CreateMeshData(
        std::span<const std::byte> vertexBytes,
        std::size_t vertexStrideBytes,
        MeshVertexLayout layout,
        std::span<const std::uint32_t> indices,
        const Sphere3D& localBoundingSphere);
    [[nodiscard]] static Sphere3D ComputeLocalBoundingSphere(
        const Shape& shape) noexcept;
};
