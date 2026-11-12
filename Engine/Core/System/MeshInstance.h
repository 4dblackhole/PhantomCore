#pragma once

#include "Mesh/MeshRendering.h"
#include "Rendering/RenderContext.h"
#include "System/Camera.h"
#include "System/TransformNode.h"

#include "../../Math/Math.h"

#include <cstdint>


// Reusable wrapper that combines one GPU mesh, one material,
// and per-instance state.  Shape itself stays renderer-independent.
namespace PhantomCore
{
class MeshInstance final
{
public:
    MeshInstance() = default;
    MeshInstance(
        GpuMeshHandle mesh,
        MaterialInstanceHandle material);

    void SetMesh(GpuMeshHandle mesh);
    void SetMaterial(MaterialInstanceHandle material);
    void Reset() noexcept;
    [[nodiscard]] const GpuMeshHandle& Mesh() const noexcept;
    [[nodiscard]] const MaterialInstanceHandle&
        Material() const noexcept;

    [[nodiscard]] TransformNode& Transform() noexcept;
    [[nodiscard]] const TransformNode& Transform() const noexcept;

    void SetColor(const Float4& color) noexcept;
    [[nodiscard]] const Float4& Color() const noexcept;
    void SetTextureIndex(std::uint32_t textureIndex) noexcept;
    void ClearTexture() noexcept;
    [[nodiscard]] std::uint32_t TextureIndex() const noexcept;
    void SetUvTransform(
        const Float2& scale,
        const Float2& offset) noexcept;
    void SetUvTransform(
        const UvTransform& transform) noexcept;
    [[nodiscard]] const Float2& UvScale() const noexcept;
    [[nodiscard]] const Float2& UvOffset() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] const Sphere3D&
        LocalBoundingSphere() const;
    [[nodiscard]] Sphere3D WorldBoundingSphere() const;
    [[nodiscard]] bool IsVisible(const Camera& camera) const;

    // Queues this instance when its world bounding sphere is not outside
    // the camera frustum. D3D12Renderer batches matching mesh/material
    // pairs and performs DrawIndexedInstanced in EndFrame.
    void Submit(
        const RenderContext& context,
        const Camera& camera);

private:
    GpuMeshHandle mesh_;
    MaterialInstanceHandle material_;
    TransformNode transform_;
    Float4 color_{1.0F, 1.0F, 1.0F, 1.0F};
    Float2 uvScale_{1.0F, 1.0F};
    Float2 uvOffset_{0.0F, 0.0F};
    std::uint32_t textureIndex_{NoTextureIndex};
};

} // namespace PhantomCore
