#include "Visual2D/Visual2DSurface.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>


namespace PhantomCore
{
namespace
{
    using namespace DirectX;

    struct LocalRay
    {
        Ray3D ray{};
        XMMATRIX world{};
        XMMATRIX inverseWorld{};
    };

    [[nodiscard]] std::optional<LocalRay> ToLocalRay(
        const Ray3D& worldRay,
        const XMFLOAT4X4& worldTransform) noexcept
    {
        const XMMATRIX world = XMLoadFloat4x4(&worldTransform);
        XMVECTOR determinant{};
        const XMMATRIX inverse = XMMatrixInverse(&determinant, world);
        if (std::abs(XMVectorGetX(determinant)) <=
            std::numeric_limits<float>::epsilon())
        {
            return std::nullopt;
        }

        const XMVECTOR origin = XMVector3TransformCoord(
            XMLoadFloat3(&worldRay.origin), inverse);
        const XMVECTOR direction = XMVector3TransformNormal(
            XMLoadFloat3(&worldRay.direction), inverse);
        if (XMVectorGetX(XMVector3LengthSq(direction)) <=
            std::numeric_limits<float>::epsilon())
        {
            return std::nullopt;
        }

        LocalRay result{};
        XMStoreFloat3(&result.ray.origin, origin);
        XMStoreFloat3(&result.ray.direction, direction);
        result.world = world;
        result.inverseWorld = inverse;
        return result;
    }

    [[nodiscard]] XMFLOAT3 TransformNormal(
        const XMFLOAT3& localNormal,
        const XMMATRIX& inverseWorld) noexcept
    {
        XMVECTOR normal = XMVector3TransformNormal(
            XMLoadFloat3(&localNormal),
            XMMatrixTranspose(inverseWorld));
        normal = XMVector3Normalize(normal);
        XMFLOAT3 result{};
        XMStoreFloat3(&result, normal);
        return result;
    }

    [[nodiscard]] float Dot(
        const XMFLOAT3& first,
        const XMFLOAT3& second) noexcept
    {
        return first.x * second.x + first.y * second.y +
            first.z * second.z;
    }

    [[nodiscard]] float AxisValue(
        const XMFLOAT3& value,
        const std::size_t axis) noexcept
    {
        if (axis == 0)
        {
            return value.x;
        }
        if (axis == 1)
        {
            return value.y;
        }
        return value.z;
    }

    void ExpandBounds(
        XMFLOAT3& minimum,
        XMFLOAT3& maximum,
        const XMFLOAT3& point) noexcept
    {
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        minimum.z = std::min(minimum.z, point.z);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        maximum.z = std::max(maximum.z, point.z);
    }

    [[nodiscard]] bool IntersectsBounds(
        const XMFLOAT3& minimum,
        const XMFLOAT3& maximum,
        const Ray3D& ray,
        const float maximumParameter) noexcept
    {
        float nearParameter = 0.0F;
        float farParameter = maximumParameter;
        constexpr float ParallelEpsilon = 1.0e-8F;

        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const float origin = AxisValue(ray.origin, axis);
            const float direction = AxisValue(ray.direction, axis);
            const float slabMinimum = AxisValue(minimum, axis);
            const float slabMaximum = AxisValue(maximum, axis);
            if (std::abs(direction) <= ParallelEpsilon)
            {
                if (origin < slabMinimum || origin > slabMaximum)
                {
                    return false;
                }
                continue;
            }

            float first = (slabMinimum - origin) / direction;
            float second = (slabMaximum - origin) / direction;
            if (first > second)
            {
                std::swap(first, second);
            }
            nearParameter = std::max(nearParameter, first);
            farParameter = std::min(farParameter, second);
            if (farParameter < nearParameter)
            {
                return false;
            }
        }
        return true;
    }
}

IVisual2DSurface::~IVisual2DSurface() = default;

PlaneVisual2DSurface::PlaneVisual2DSurface(
    const float width,
    const float height,
    const DirectX::XMFLOAT4X4& worldTransform,
    const bool twoSided)
    : width_(width), height_(height),
      worldTransform_(worldTransform), twoSided_(twoSided)
{
    if (!std::isfinite(width_) || !std::isfinite(height_) ||
        width_ <= 0.0F || height_ <= 0.0F)
    {
        throw std::invalid_argument(
            "A plane UI surface requires a finite positive size.");
    }
}

void PlaneVisual2DSurface::SetWorldTransform(
    const DirectX::XMFLOAT4X4& worldTransform) noexcept
{
    worldTransform_ = worldTransform;
}

const DirectX::XMFLOAT4X4& PlaneVisual2DSurface::WorldTransform() const noexcept
{
    return worldTransform_;
}

Size PlaneVisual2DSurface::WorldSize() const noexcept
{
    return {width_, height_};
}

std::optional<SurfaceHit> PlaneVisual2DSurface::Raycast(
    const Ray3D& worldRay) const noexcept
{
    const std::optional<LocalRay> local =
        ToLocalRay(worldRay, worldTransform_);
    if (!local.has_value())
    {
        return std::nullopt;
    }
    const Plane3D plane{{0.0F, 0.0F, -1.0F}, 0.0F};
    const std::optional<LineHit3D> hit =
        Intersect(plane, local->ray);
    if (!hit.has_value() ||
        (!twoSided_ && Dot(hit->normal, local->ray.direction) >= 0.0F))
    {
        return std::nullopt;
    }

    const float halfWidth = width_ * 0.5F;
    const float halfHeight = height_ * 0.5F;
    if (hit->point.x < -halfWidth || hit->point.x > halfWidth ||
        hit->point.y < -halfHeight || hit->point.y > halfHeight)
    {
        return std::nullopt;
    }

    DirectX::XMFLOAT3 worldPoint{};
    DirectX::XMStoreFloat3(
        &worldPoint,
        DirectX::XMVector3TransformCoord(
            DirectX::XMLoadFloat3(&hit->point), local->world));
    return SurfaceHit{
        hit->parameter,
        worldPoint,
        TransformNormal(hit->normal, local->inverseWorld),
        {(hit->point.x + halfWidth) / width_,
            (halfHeight - hit->point.y) / height_}};
}

MeshUvVisual2DSurface::MeshUvVisual2DSurface(
    const Shape& shape,
    const DirectX::XMFLOAT4X4& worldTransform,
    const bool twoSided)
    : worldTransform_(worldTransform), twoSided_(twoSided)
{
    vertices_.reserve(shape.VertexCount());
    for (const VertexAttributes& vertex : shape.Vertices())
    {
        vertices_.push_back({vertex.position, vertex.uv});
    }
    indices_.assign(shape.Indices().begin(), shape.Indices().end());
    if (vertices_.empty() || indices_.empty() ||
        indices_.size() % 3 != 0)
    {
        throw std::invalid_argument(
            "A mesh UI surface requires indexed triangles.");
    }
    if (std::ranges::any_of(indices_, [this](const std::uint32_t index)
        {
            return index >= vertices_.size();
        }))
    {
        throw std::invalid_argument(
            "A mesh UI surface contains an invalid vertex index.");
    }
    BuildBvh();
}

void MeshUvVisual2DSurface::BuildBvh()
{
    const std::size_t triangleCount = indices_.size() / 3;
    triangleOrder_.resize(triangleCount);
    std::iota(triangleOrder_.begin(), triangleOrder_.end(), 0);
    bvhNodes_.clear();
    bvhNodes_.reserve(triangleCount * 2);
    static_cast<void>(BuildBvhNode(0, triangleCount));
}

std::size_t MeshUvVisual2DSurface::BuildBvhNode(
    const std::size_t firstTriangle,
    const std::size_t triangleCount)
{
    constexpr std::size_t LeafTriangleCount = 6;
    const float infinity = std::numeric_limits<float>::infinity();
    Bounds bounds{{infinity, infinity, infinity},
        {-infinity, -infinity, -infinity}};
    DirectX::XMFLOAT3 centroidMinimum{infinity, infinity, infinity};
    DirectX::XMFLOAT3 centroidMaximum{-infinity, -infinity, -infinity};

    for (std::size_t offset = 0; offset < triangleCount; ++offset)
    {
        const std::size_t triangle =
            triangleOrder_[firstTriangle + offset];
        const std::size_t index = triangle * 3;
        const DirectX::XMFLOAT3& first =
            vertices_[indices_[index]].position;
        const DirectX::XMFLOAT3& second =
            vertices_[indices_[index + 1]].position;
        const DirectX::XMFLOAT3& third =
            vertices_[indices_[index + 2]].position;
        ExpandBounds(bounds.minimum, bounds.maximum, first);
        ExpandBounds(bounds.minimum, bounds.maximum, second);
        ExpandBounds(bounds.minimum, bounds.maximum, third);
        const DirectX::XMFLOAT3 centroid{
            (first.x + second.x + third.x) / 3.0F,
            (first.y + second.y + third.y) / 3.0F,
            (first.z + second.z + third.z) / 3.0F};
        ExpandBounds(centroidMinimum, centroidMaximum, centroid);
    }

    const std::size_t nodeIndex = bvhNodes_.size();
    bvhNodes_.push_back(BvhNode{bounds});
    const DirectX::XMFLOAT3 centroidExtent{
        centroidMaximum.x - centroidMinimum.x,
        centroidMaximum.y - centroidMinimum.y,
        centroidMaximum.z - centroidMinimum.z};
    std::size_t splitAxis = 0;
    if (centroidExtent.y > centroidExtent.x)
    {
        splitAxis = 1;
    }
    if (AxisValue(centroidExtent, 2) >
        AxisValue(centroidExtent, splitAxis))
    {
        splitAxis = 2;
    }

    if (triangleCount <= LeafTriangleCount ||
        AxisValue(centroidExtent, splitAxis) <=
            std::numeric_limits<float>::epsilon())
    {
        bvhNodes_[nodeIndex].firstTriangle = firstTriangle;
        bvhNodes_[nodeIndex].triangleCount = triangleCount;
        bvhNodes_[nodeIndex].escapeIndex = nodeIndex + 1;
        return nodeIndex;
    }

    const std::size_t leftCount = triangleCount / 2;
    const auto firstIterator =
        triangleOrder_.begin() + firstTriangle;
    const auto middleIterator = firstIterator + leftCount;
    const auto lastIterator = firstIterator + triangleCount;
    std::nth_element(
        firstIterator,
        middleIterator,
        lastIterator,
        [this, splitAxis](
            const std::size_t firstTriangleIndex,
            const std::size_t secondTriangleIndex)
        {
            const auto centroidAxis = [this, splitAxis](
                const std::size_t triangle)
            {
                const std::size_t index = triangle * 3;
                return (
                    AxisValue(
                        vertices_[indices_[index]].position,
                        splitAxis) +
                    AxisValue(
                        vertices_[indices_[index + 1]].position,
                        splitAxis) +
                    AxisValue(
                        vertices_[indices_[index + 2]].position,
                        splitAxis)) / 3.0F;
            };
            return centroidAxis(firstTriangleIndex) <
                centroidAxis(secondTriangleIndex);
        });

    static_cast<void>(BuildBvhNode(firstTriangle, leftCount));
    static_cast<void>(BuildBvhNode(
        firstTriangle + leftCount,
        triangleCount - leftCount));
    bvhNodes_[nodeIndex].escapeIndex = bvhNodes_.size();
    return nodeIndex;
}

void MeshUvVisual2DSurface::SetWorldTransform(
    const DirectX::XMFLOAT4X4& worldTransform) noexcept
{
    worldTransform_ = worldTransform;
}

const DirectX::XMFLOAT4X4& MeshUvVisual2DSurface::WorldTransform() const noexcept
{
    return worldTransform_;
}

std::optional<SurfaceHit> MeshUvVisual2DSurface::Raycast(
    const Ray3D& worldRay) const noexcept
{
    const std::optional<LocalRay> local =
        ToLocalRay(worldRay, worldTransform_);
    if (!local.has_value())
    {
        return std::nullopt;
    }

    std::optional<TriangleHit3D> closest;
    const SurfaceVertex* closestVertices[3]{};
    float closestParameter = std::numeric_limits<float>::infinity();
    std::size_t nodeIndex = 0;
    while (nodeIndex < bvhNodes_.size())
    {
        const BvhNode& node = bvhNodes_[nodeIndex];
        if (!IntersectsBounds(
            node.bounds.minimum,
            node.bounds.maximum,
            local->ray,
            closestParameter))
        {
            nodeIndex = node.escapeIndex;
            continue;
        }

        // Internal nodes are immediately followed by their left child in
        // preorder. Leaves jump to escapeIndex after testing their range.
        if (node.triangleCount == 0)
        {
            ++nodeIndex;
            continue;
        }

        for (std::size_t offset = 0;
             offset < node.triangleCount;
             ++offset)
        {
            const std::size_t triangleIndex =
                triangleOrder_[node.firstTriangle + offset];
            const std::size_t index = triangleIndex * 3;
            const SurfaceVertex& first = vertices_[indices_[index]];
            const SurfaceVertex& second =
                vertices_[indices_[index + 1]];
            const SurfaceVertex& third =
                vertices_[indices_[index + 2]];
            const Triangle3D triangle{
                first.position, second.position, third.position};
            const std::optional<TriangleHit3D> hit =
                Intersect(triangle, local->ray);
            if (!hit.has_value() ||
                (!twoSided_ &&
                    Dot(hit->normal, local->ray.direction) >= 0.0F) ||
                hit->parameter >= closestParameter)
            {
                continue;
            }
            closest = hit;
            closestParameter = hit->parameter;
            closestVertices[0] = &first;
            closestVertices[1] = &second;
            closestVertices[2] = &third;
        }
        nodeIndex = node.escapeIndex;
    }
    if (!closest.has_value())
    {
        return std::nullopt;
    }

    const DirectX::XMFLOAT3 weights = closest->barycentric;
    const DirectX::XMFLOAT2 uv{
        closestVertices[0]->uv.x * weights.x +
            closestVertices[1]->uv.x * weights.y +
            closestVertices[2]->uv.x * weights.z,
        closestVertices[0]->uv.y * weights.x +
            closestVertices[1]->uv.y * weights.y +
            closestVertices[2]->uv.y * weights.z};
    DirectX::XMFLOAT3 worldPoint{};
    DirectX::XMStoreFloat3(
        &worldPoint,
        DirectX::XMVector3TransformCoord(
            DirectX::XMLoadFloat3(&closest->point), local->world));
    return SurfaceHit{
        closest->parameter,
        worldPoint,
        TransformNormal(closest->normal, local->inverseWorld),
        uv};
}

WorldSpaceVisual2DCanvas::WorldSpaceVisual2DCanvas(
    const Size logicalSize,
    std::unique_ptr<IVisual2DSurface> surface)
    : canvas_(logicalSize, CanvasScaleMode::Fixed),
      surface_(std::move(surface))
{
    if (surface_ == nullptr)
    {
        throw std::invalid_argument(
            "A world-space canvas requires a UI surface.");
    }
}

Visual2DCanvas& WorldSpaceVisual2DCanvas::Canvas() noexcept
{
    return canvas_;
}

const Visual2DCanvas& WorldSpaceVisual2DCanvas::Canvas() const noexcept
{
    return canvas_;
}

IVisual2DSurface& WorldSpaceVisual2DCanvas::Surface() noexcept
{
    return *surface_;
}

const IVisual2DSurface& WorldSpaceVisual2DCanvas::Surface() const noexcept
{
    return *surface_;
}

void WorldSpaceVisual2DCanvas::SetSurface(std::unique_ptr<IVisual2DSurface> surface)
{
    if (surface == nullptr)
    {
        throw std::invalid_argument("A UI surface cannot be null.");
    }
    surface_ = std::move(surface);
}

std::optional<Point> WorldSpaceVisual2DCanvas::MapPointer(
    const Ray3D& worldRay) const noexcept
{
    const std::optional<SurfaceHit> hit = surface_->Raycast(worldRay);
    if (!hit.has_value())
    {
        return std::nullopt;
    }
    const Size size = canvas_.LogicalSize();
    return Point{
        (hit->uv.x - 0.5F) * size.width,
        (0.5F - hit->uv.y) * size.height};
}

std::optional<Ray3D> CreateWorldPointerRay(
    const Point screenPosition,
    const Size viewportSize,
    const DirectX::XMFLOAT4X4& viewProjection) noexcept
{
    if (viewportSize.width <= 0.0F || viewportSize.height <= 0.0F)
    {
        return std::nullopt;
    }
    const float normalizedX =
        screenPosition.x * 2.0F / viewportSize.width - 1.0F;
    const float normalizedY =
        1.0F - screenPosition.y * 2.0F / viewportSize.height;

    DirectX::XMVECTOR determinant{};
    const DirectX::XMMATRIX inverse = DirectX::XMMatrixInverse(
        &determinant,
        DirectX::XMLoadFloat4x4(&viewProjection));
    if (std::abs(DirectX::XMVectorGetX(determinant)) <=
        std::numeric_limits<float>::epsilon())
    {
        return std::nullopt;
    }

    const DirectX::XMVECTOR nearPoint = DirectX::XMVector3TransformCoord(
        DirectX::XMVectorSet(normalizedX, normalizedY, 0.0F, 1.0F),
        inverse);
    const DirectX::XMVECTOR farPoint = DirectX::XMVector3TransformCoord(
        DirectX::XMVectorSet(normalizedX, normalizedY, 1.0F, 1.0F),
        inverse);
    DirectX::XMVECTOR direction = DirectX::XMVectorSubtract(
        farPoint, nearPoint);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(direction)) <=
        std::numeric_limits<float>::epsilon())
    {
        return std::nullopt;
    }
    direction = DirectX::XMVector3Normalize(direction);

    Ray3D result{};
    DirectX::XMStoreFloat3(&result.origin, nearPoint);
    DirectX::XMStoreFloat3(&result.direction, direction);
    return result;
}

} // namespace PhantomCore
