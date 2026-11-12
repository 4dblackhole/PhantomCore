#pragma once

#include "Visual2D/Visual2DCanvas.h"
#include "Query/Collision.h"
#include "Shape/Shape.h"

#include "../../Math/Math.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>


namespace PhantomCore
{
struct SurfaceHit
{
    float rayParameter{};
    Float3 worldPosition{};
    Float3 worldNormal{};
    Float2 uv{};
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
        const Float4x4& worldTransform,
        bool twoSided = false);

    void SetWorldTransform(
        const Float4x4& worldTransform) noexcept;
    [[nodiscard]] const Float4x4& WorldTransform()
        const noexcept;
    [[nodiscard]] Size WorldSize() const noexcept;
    [[nodiscard]] std::optional<SurfaceHit> Raycast(
        const Ray3D& worldRay) const noexcept override;

private:
    float width_{};
    float height_{};
    Float4x4 worldTransform_{};
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
        const Float4x4& worldTransform,
        bool twoSided = false);

    void SetWorldTransform(
        const Float4x4& worldTransform) noexcept;
    [[nodiscard]] const Float4x4& WorldTransform()
        const noexcept;
    [[nodiscard]] std::optional<SurfaceHit> Raycast(
        const Ray3D& worldRay) const noexcept override;

private:
    struct SurfaceVertex
    {
        Float3 position{};
        Float2 uv{};
    };

    struct Bounds
    {
        Float3 minimum{};
        Float3 maximum{};
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
    Float4x4 worldTransform_{};
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
    const Float4x4& viewProjection) noexcept;

} // namespace PhantomCore
