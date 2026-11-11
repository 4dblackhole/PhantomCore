#pragma once

#include "Visual2D/Visual2DCanvas.h"
#include "Query/Collision.h"
#include "Shape/Shape.h"

#include <DirectXMath.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>


namespace PhantomCore
{
struct SurfaceHit
{
    float rayParameter{};
    DirectX::XMFLOAT3 worldPosition{};
    DirectX::XMFLOAT3 worldNormal{};
    DirectX::XMFLOAT2 uv{};
};

class IVisual2DSurface
{
public:
    virtual ~IVisual2DSurface();
    [[nodiscard]] virtual std::optional<SurfaceHit> Raycast(
        const Ray3D& worldRay) const noexcept = 0;
};

// Finite XY plane. Local UV (0,0) remains the upper-left texture corner;
// WorldSpaceVisual2DCanvas converts it to centered, Y-up Canvas space.
class PlaneVisual2DSurface final : public IVisual2DSurface
{
public:
    PlaneVisual2DSurface(
        float width,
        float height,
        const DirectX::XMFLOAT4X4& worldTransform,
        bool twoSided = false);

    void SetWorldTransform(
        const DirectX::XMFLOAT4X4& worldTransform) noexcept;
    [[nodiscard]] const DirectX::XMFLOAT4X4& WorldTransform()
        const noexcept;
    [[nodiscard]] Size WorldSize() const noexcept;
    [[nodiscard]] std::optional<SurfaceHit> Raycast(
        const Ray3D& worldRay) const noexcept override;

private:
    float width_{};
    float height_{};
    DirectX::XMFLOAT4X4 worldTransform_{};
    bool twoSided_{};
};

// Copies CPU positions/UVs from a Shape and builds an immutable local-space
// BVH. Ray queries skip unrelated triangle groups without changing the
// surface or Canvas API, including for rotated and curved meshes.
class MeshUvVisual2DSurface final : public IVisual2DSurface
{
public:
    MeshUvVisual2DSurface(
        const Shape& shape,
        const DirectX::XMFLOAT4X4& worldTransform,
        bool twoSided = false);

    void SetWorldTransform(
        const DirectX::XMFLOAT4X4& worldTransform) noexcept;
    [[nodiscard]] const DirectX::XMFLOAT4X4& WorldTransform()
        const noexcept;
    [[nodiscard]] std::optional<SurfaceHit> Raycast(
        const Ray3D& worldRay) const noexcept override;

private:
    struct SurfaceVertex
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT2 uv{};
    };

    struct Bounds
    {
        DirectX::XMFLOAT3 minimum{};
        DirectX::XMFLOAT3 maximum{};
    };

    // Nodes are stored in preorder. escapeIndex points immediately after
    // the subtree, allowing Raycast to traverse without a stack or a
    // per-query allocation.
    struct BvhNode
    {
        Bounds bounds{};
        std::size_t firstTriangle{};
        std::size_t triangleCount{};
        std::size_t escapeIndex{};
    };

    void BuildBvh();
    [[nodiscard]] std::size_t BuildBvhNode(
        std::size_t firstTriangle,
        std::size_t triangleCount);

    std::vector<SurfaceVertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<std::size_t> triangleOrder_;
    std::vector<BvhNode> bvhNodes_;
    DirectX::XMFLOAT4X4 worldTransform_{};
    bool twoSided_{};
};

// Owns a logical Canvas and composes it with a replaceable world surface.
// It does not inherit Visual2DCanvas because presentation is not content
// ownership.
class WorldSpaceVisual2DCanvas final
{
public:
    WorldSpaceVisual2DCanvas(
        Size logicalSize,
        std::unique_ptr<IVisual2DSurface> surface);

    [[nodiscard]] Visual2DCanvas& Canvas() noexcept;
    [[nodiscard]] const Visual2DCanvas& Canvas() const noexcept;
    [[nodiscard]] IVisual2DSurface& Surface() noexcept;
    [[nodiscard]] const IVisual2DSurface& Surface() const noexcept;
    void SetSurface(std::unique_ptr<IVisual2DSurface> surface);
    [[nodiscard]] std::optional<Point> MapPointer(
        const Ray3D& worldRay) const noexcept;

private:
    Visual2DCanvas canvas_;
    std::unique_ptr<IVisual2DSurface> surface_;
};

[[nodiscard]] std::optional<Ray3D> CreateWorldPointerRay(
    Point screenPosition,
    Size viewportSize,
    const DirectX::XMFLOAT4X4& viewProjection) noexcept;

} // namespace PhantomCore
