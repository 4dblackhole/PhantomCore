#pragma once

// Backend-neutral 2D/3D collision primitives and stateless intersection
// queries. All positions and sizes use engine world units.

#include <DirectXMath.h>

#include <cstdint>
#include <optional>


namespace PhantomCore
{
inline constexpr float DefaultEpsilon = 1.0e-5F;

struct Line2D
{
    DirectX::XMFLOAT2 point{};
    DirectX::XMFLOAT2 direction{1.0F, 0.0F};
};

struct LineSegment2D
{
    DirectX::XMFLOAT2 start{};
    DirectX::XMFLOAT2 end{};
};

struct Circle2D
{
    DirectX::XMFLOAT2 center{};
    float radius{};
};

// rotationRadians rotates the local X axis counter-clockwise.
struct Obb2D
{
    DirectX::XMFLOAT2 center{};
    DirectX::XMFLOAT2 halfExtents{0.5F, 0.5F};
    float rotationRadians{};
};

struct Line3D
{
    DirectX::XMFLOAT3 point{};
    DirectX::XMFLOAT3 direction{1.0F, 0.0F, 0.0F};
};

struct Ray3D
{
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{1.0F, 0.0F, 0.0F};
};

struct LineSegment3D
{
    DirectX::XMFLOAT3 start{};
    DirectX::XMFLOAT3 end{};
};

// Plane equation: dot(normal, point) = distanceFromOrigin.
// The normal does not have to be normalized.
struct Plane3D
{
    DirectX::XMFLOAT3 normal{0.0F, 1.0F, 0.0F};
    float distanceFromOrigin{};
};

struct Sphere3D
{
    DirectX::XMFLOAT3 center{};
    float radius{};
};

// All normals point into the visible volume. The planes use the same
// dot(normal, point) = distanceFromOrigin convention as Plane3D.
struct ViewFrustum
{
    Plane3D left{{}, 0.0F};
    Plane3D right{{}, 0.0F};
    Plane3D bottom{{}, 0.0F};
    Plane3D top{{}, 0.0F};
    Plane3D nearPlane{{}, 0.0F};
    Plane3D farPlane{{}, 0.0F};
};

enum class VolumeIntersection : std::uint8_t
{
    Outside,
    Intersecting,
    Inside,
};

struct Triangle3D
{
    DirectX::XMFLOAT3 first{};
    DirectX::XMFLOAT3 second{};
    DirectX::XMFLOAT3 third{};
};

// orientation is a quaternion in (x, y, z, w) order. It is normalized
// internally before a query.
struct Obb3D
{
    DirectX::XMFLOAT3 center{};
    DirectX::XMFLOAT3 halfExtents{0.5F, 0.5F, 0.5F};
    DirectX::XMFLOAT4 orientation{0.0F, 0.0F, 0.0F, 1.0F};
};

struct LineHit3D
{
    DirectX::XMFLOAT3 point{};
    DirectX::XMFLOAT3 normal{};

    // point + direction * parameter for Line3D, origin + direction *
    // parameter for Ray3D, and lerp(start, end, parameter) for a segment.
    // Segment parameters are clamped to [0, 1].
    float parameter{};
};

struct TriangleHit3D
{
    DirectX::XMFLOAT3 point{};
    DirectX::XMFLOAT3 normal{};
    // Weights for first, second, and third. They sum to one and can be
    // used to interpolate UVs or other per-vertex attributes.
    DirectX::XMFLOAT3 barycentric{};
    float parameter{};
};

// Extracts a left-handed DirectX frustum (x/y in [-w, w], z in [0, w])
// from a row-vector view-projection matrix. Extracted planes are normalized.
[[nodiscard]] ViewFrustum MakeViewFrustum(
    const DirectX::XMFLOAT4X4& viewProjection) noexcept;

// Transforms a sphere by an affine world matrix. The radius uses the
// largest world-axis scale and falls back to a conservative matrix-norm
// bound when a transform hierarchy introduces shear.
[[nodiscard]] Sphere3D TransformSphere(
    const Sphere3D& sphere,
    const DirectX::XMFLOAT4X4& world) noexcept;

[[nodiscard]] VolumeIntersection Classify(
    const ViewFrustum& frustum,
    const Sphere3D& sphere,
    float epsilon = DefaultEpsilon) noexcept;

[[nodiscard]] DirectX::XMFLOAT2 ClosestPoint(
    const LineSegment2D& segment,
    const DirectX::XMFLOAT2& point) noexcept;
[[nodiscard]] DirectX::XMFLOAT2 ClosestPoint(
    const Obb2D& box,
    const DirectX::XMFLOAT2& point) noexcept;

[[nodiscard]] bool Intersects(
    const Circle2D& circle,
    const Line2D& line,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Circle2D& circle,
    const LineSegment2D& segment,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Circle2D& circle,
    const Obb2D& box,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Obb2D& first,
    const Obb2D& second,
    float epsilon = DefaultEpsilon) noexcept;

[[nodiscard]] bool Intersects(
    const Line2D& line,
    const Circle2D& circle,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const LineSegment2D& segment,
    const Circle2D& circle,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Obb2D& box,
    const Circle2D& circle,
    float epsilon = DefaultEpsilon) noexcept;

// A coplanar line/ray/segment returns a representative hit at parameter
// zero. Parallel objects on separate planes return std::nullopt.
[[nodiscard]] std::optional<LineHit3D> Intersect(
    const Plane3D& plane,
    const Line3D& line,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] std::optional<LineHit3D> Intersect(
    const Plane3D& plane,
    const Ray3D& ray,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] std::optional<LineHit3D> Intersect(
    const Plane3D& plane,
    const LineSegment3D& segment,
    float epsilon = DefaultEpsilon) noexcept;

[[nodiscard]] bool Intersects(
    const Plane3D& plane,
    const Line3D& line,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Plane3D& plane,
    const Ray3D& ray,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Plane3D& plane,
    const LineSegment3D& segment,
    float epsilon = DefaultEpsilon) noexcept;

// Moller-Trumbore ray/triangle query. Winding is preserved in the
// returned normal; callers decide whether to reject a back face.
[[nodiscard]] std::optional<TriangleHit3D> Intersect(
    const Triangle3D& triangle,
    const Ray3D& ray,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Triangle3D& triangle,
    const Ray3D& ray,
    float epsilon = DefaultEpsilon) noexcept;

[[nodiscard]] bool Intersects(
    const Sphere3D& sphere,
    const Line3D& line,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Sphere3D& sphere,
    const Ray3D& ray,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Sphere3D& sphere,
    const LineSegment3D& segment,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Sphere3D& sphere,
    const Obb3D& box,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Obb3D& first,
    const Obb3D& second,
    float epsilon = DefaultEpsilon) noexcept;

[[nodiscard]] bool Intersects(
    const Line3D& line,
    const Sphere3D& sphere,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Ray3D& ray,
    const Sphere3D& sphere,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const LineSegment3D& segment,
    const Sphere3D& sphere,
    float epsilon = DefaultEpsilon) noexcept;
[[nodiscard]] bool Intersects(
    const Obb3D& box,
    const Sphere3D& sphere,
    float epsilon = DefaultEpsilon) noexcept;

} // namespace PhantomCore
