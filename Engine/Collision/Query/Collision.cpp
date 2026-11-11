#include "Query/Collision.h"

#include <DirectXCollision.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace PhantomCore
{
namespace
{
    using DirectX::XMFLOAT2;
    using DirectX::XMFLOAT3;
    using DirectX::XMFLOAT4;

    [[nodiscard]] float AbsEpsilon(const float epsilon) noexcept
    {
        return std::isfinite(epsilon)
            ? std::abs(epsilon)
            : DefaultEpsilon;
    }

    [[nodiscard]] float Dot(
        const XMFLOAT2& first,
        const XMFLOAT2& second) noexcept
    {
        return first.x * second.x + first.y * second.y;
    }

    [[nodiscard]] float Dot(
        const XMFLOAT3& first,
        const XMFLOAT3& second) noexcept
    {
        return first.x * second.x +
            first.y * second.y +
            first.z * second.z;
    }

    [[nodiscard]] XMFLOAT2 Subtract(
        const XMFLOAT2& first,
        const XMFLOAT2& second) noexcept
    {
        return {first.x - second.x, first.y - second.y};
    }

    [[nodiscard]] XMFLOAT3 Subtract(
        const XMFLOAT3& first,
        const XMFLOAT3& second) noexcept
    {
        return {
            first.x - second.x,
            first.y - second.y,
            first.z - second.z};
    }

    [[nodiscard]] XMFLOAT3 AddScaled(
        const XMFLOAT3& point,
        const XMFLOAT3& direction,
        const float parameter) noexcept
    {
        return {
            point.x + direction.x * parameter,
            point.y + direction.y * parameter,
            point.z + direction.z * parameter};
    }

    [[nodiscard]] XMFLOAT3 Cross(
        const XMFLOAT3& first,
        const XMFLOAT3& second) noexcept
    {
        return {
            first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
    }

    [[nodiscard]] float LengthSquared(const XMFLOAT2& value) noexcept
    {
        return Dot(value, value);
    }

    [[nodiscard]] float LengthSquared(const XMFLOAT3& value) noexcept
    {
        return Dot(value, value);
    }

    [[nodiscard]] bool HasValidRadius(const float radius) noexcept
    {
        return std::isfinite(radius) && radius >= 0.0F;
    }

    [[nodiscard]] bool HasFiniteCoordinates(const XMFLOAT3& value) noexcept
    {
        return std::isfinite(value.x) &&
            std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    [[nodiscard]] bool TryMakeFrustumPlane(
        const float x,
        const float y,
        const float z,
        const float constant,
        Plane3D& plane) noexcept
    {
        const float lengthSquared = x * x + y * y + z * z;
        if (!std::isfinite(lengthSquared) ||
            !std::isfinite(constant) ||
            lengthSquared <= std::numeric_limits<float>::epsilon())
        {
            plane = {{}, 0.0F};
            return false;
        }

        const float inverseLength = 1.0F / std::sqrt(lengthSquared);
        plane.normal = {
            x * inverseLength,
            y * inverseLength,
            z * inverseLength};
        plane.distanceFromOrigin = -constant * inverseLength;
        return true;
    }

    [[nodiscard]] float MaximumWorldScale(
        const DirectX::XMFLOAT4X4& world) noexcept
    {
        const std::array<XMFLOAT3, 3> axes{
            XMFLOAT3{world._11, world._12, world._13},
            XMFLOAT3{world._21, world._22, world._23},
            XMFLOAT3{world._31, world._32, world._33}};
        const std::array<float, 3> lengthSquared{
            LengthSquared(axes[0]),
            LengthSquared(axes[1]),
            LengthSquared(axes[2])};
        if (!std::isfinite(lengthSquared[0]) ||
            !std::isfinite(lengthSquared[1]) ||
            !std::isfinite(lengthSquared[2]))
        {
            return std::numeric_limits<float>::quiet_NaN();
        }

        const float maximumAxisScale = std::sqrt(
            std::max({lengthSquared[0], lengthSquared[1], lengthSquared[2]}));
        const auto axesAreOrthogonal =
            [&axes, &lengthSquared](const std::size_t first,
                const std::size_t second) noexcept
            {
                const float threshold = DefaultEpsilon *
                    std::sqrt(
                        lengthSquared[first] * lengthSquared[second]);
                return std::abs(Dot(axes[first], axes[second])) <= threshold;
            };
        if (axesAreOrthogonal(0, 1) &&
            axesAreOrthogonal(0, 2) &&
            axesAreOrthogonal(1, 2))
        {
            return maximumAxisScale;
        }

        // sqrt(||A||1 * ||A||inf) bounds the largest singular value and is
        // therefore safe for hierarchy-induced shear.
        const float maximumRowSum = std::max({
            std::abs(world._11) + std::abs(world._12) + std::abs(world._13),
            std::abs(world._21) + std::abs(world._22) + std::abs(world._23),
            std::abs(world._31) + std::abs(world._32) + std::abs(world._33)});
        const float maximumColumnSum = std::max({
            std::abs(world._11) + std::abs(world._21) + std::abs(world._31),
            std::abs(world._12) + std::abs(world._22) + std::abs(world._32),
            std::abs(world._13) + std::abs(world._23) + std::abs(world._33)});
        return std::sqrt(maximumRowSum * maximumColumnSum);
    }

    [[nodiscard]] bool HasValidExtents(const XMFLOAT2& extents) noexcept
    {
        return std::isfinite(extents.x) &&
            std::isfinite(extents.y) &&
            extents.x >= 0.0F &&
            extents.y >= 0.0F;
    }

    [[nodiscard]] bool HasValidExtents(const XMFLOAT3& extents) noexcept
    {
        return std::isfinite(extents.x) &&
            std::isfinite(extents.y) &&
            std::isfinite(extents.z) &&
            extents.x >= 0.0F &&
            extents.y >= 0.0F &&
            extents.z >= 0.0F;
    }

    struct Axes2D
    {
        XMFLOAT2 x;
        XMFLOAT2 y;
    };

    [[nodiscard]] Axes2D BoxAxes(const Obb2D& box) noexcept
    {
        const float cosine = std::cos(box.rotationRadians);
        const float sine = std::sin(box.rotationRadians);
        return {{cosine, sine}, {-sine, cosine}};
    }

    [[nodiscard]] float ProjectionRadius(
        const Obb2D& box,
        const Axes2D& boxAxes,
        const XMFLOAT2& axis) noexcept
    {
        return box.halfExtents.x * std::abs(Dot(boxAxes.x, axis)) +
            box.halfExtents.y * std::abs(Dot(boxAxes.y, axis));
    }

    [[nodiscard]] bool IsSeparatedOnAxis(
        const Obb2D& first,
        const Axes2D& firstAxes,
        const Obb2D& second,
        const Axes2D& secondAxes,
        const XMFLOAT2& axis,
        const float epsilon) noexcept
    {
        const XMFLOAT2 centerDelta = Subtract(second.center, first.center);
        const float centerDistance = std::abs(Dot(centerDelta, axis));
        const float allowedDistance =
            ProjectionRadius(first, firstAxes, axis) +
            ProjectionRadius(second, secondAxes, axis) +
            epsilon;
        return centerDistance > allowedDistance;
    }

    [[nodiscard]] std::optional<LineHit3D> IntersectPlane(
        const Plane3D& plane,
        const XMFLOAT3& origin,
        const XMFLOAT3& direction,
        const float minimumParameter,
        const float maximumParameter,
        const bool allowDegenerateDirection,
        const float epsilon) noexcept
    {
        const float absoluteEpsilon = AbsEpsilon(epsilon);
        const float normalLengthSquared = LengthSquared(plane.normal);
        if (!std::isfinite(plane.distanceFromOrigin) ||
            !std::isfinite(normalLengthSquared) ||
            normalLengthSquared <= absoluteEpsilon * absoluteEpsilon)
        {
            return std::nullopt;
        }

        const float normalLength = std::sqrt(normalLengthSquared);
        const float signedDistance =
            Dot(plane.normal, origin) - plane.distanceFromOrigin;
        const float directionLengthSquared = LengthSquared(direction);

        if (!std::isfinite(directionLengthSquared) ||
            directionLengthSquared <= absoluteEpsilon * absoluteEpsilon)
        {
            if (!allowDegenerateDirection)
            {
                return std::nullopt;
            }
            if (std::abs(signedDistance) <= absoluteEpsilon * normalLength)
            {
                const XMFLOAT3 normal{
                    plane.normal.x / normalLength,
                    plane.normal.y / normalLength,
                    plane.normal.z / normalLength};
                return LineHit3D{origin, normal, 0.0F};
            }
            return std::nullopt;
        }

        const float denominator = Dot(plane.normal, direction);
        const float parallelThreshold =
            absoluteEpsilon * normalLength * std::sqrt(directionLengthSquared);
        if (std::abs(denominator) <= parallelThreshold)
        {
            if (std::abs(signedDistance) <= absoluteEpsilon * normalLength)
            {
                const XMFLOAT3 normal{
                    plane.normal.x / normalLength,
                    plane.normal.y / normalLength,
                    plane.normal.z / normalLength};
                return LineHit3D{origin, normal, 0.0F};
            }
            return std::nullopt;
        }

        float parameter = -signedDistance / denominator;
        if (parameter < minimumParameter - absoluteEpsilon ||
            parameter > maximumParameter + absoluteEpsilon)
        {
            return std::nullopt;
        }
        parameter = std::clamp(
            parameter,
            minimumParameter,
            maximumParameter);

        const XMFLOAT3 normal{
            plane.normal.x / normalLength,
            plane.normal.y / normalLength,
            plane.normal.z / normalLength};
        return LineHit3D{
            AddScaled(origin, direction, parameter),
            normal,
            parameter};
    }

    [[nodiscard]] bool SphereIntersectsDirection(
        const Sphere3D& sphere,
        const XMFLOAT3& origin,
        const XMFLOAT3& direction,
        const float minimumParameter,
        const float maximumParameter,
        const bool allowDegenerateDirection,
        const float epsilon) noexcept
    {
        if (!HasValidRadius(sphere.radius))
        {
            return false;
        }

        const float absoluteEpsilon = AbsEpsilon(epsilon);
        const float directionLengthSquared = LengthSquared(direction);
        if (!std::isfinite(directionLengthSquared) ||
            directionLengthSquared <= absoluteEpsilon * absoluteEpsilon)
        {
            if (!allowDegenerateDirection)
            {
                return false;
            }
            const XMFLOAT3 centerDelta = Subtract(origin, sphere.center);
            const float allowedRadius = sphere.radius + absoluteEpsilon;
            return LengthSquared(centerDelta) <=
                allowedRadius * allowedRadius;
        }

        const XMFLOAT3 toCenter = Subtract(sphere.center, origin);
        float parameter = Dot(toCenter, direction) / directionLengthSquared;
        parameter = std::clamp(
            parameter,
            minimumParameter,
            maximumParameter);
        const XMFLOAT3 closest = AddScaled(origin, direction, parameter);
        const XMFLOAT3 delta = Subtract(closest, sphere.center);
        const float allowedRadius = sphere.radius + absoluteEpsilon;
        return LengthSquared(delta) <= allowedRadius * allowedRadius;
    }

    [[nodiscard]] bool TryMakeBox(
        const Obb3D& source,
        DirectX::BoundingOrientedBox& destination,
        const float epsilon) noexcept
    {
        if (!HasValidExtents(source.halfExtents))
        {
            return false;
        }

        const float quaternionLengthSquared =
            source.orientation.x * source.orientation.x +
            source.orientation.y * source.orientation.y +
            source.orientation.z * source.orientation.z +
            source.orientation.w * source.orientation.w;
        const float absoluteEpsilon = AbsEpsilon(epsilon);
        if (!std::isfinite(quaternionLengthSquared) ||
            quaternionLengthSquared <= absoluteEpsilon * absoluteEpsilon)
        {
            return false;
        }

        const float inverseLength = 1.0F / std::sqrt(quaternionLengthSquared);
        destination.Center = source.center;
        destination.Extents = {
            source.halfExtents.x + absoluteEpsilon,
            source.halfExtents.y + absoluteEpsilon,
            source.halfExtents.z + absoluteEpsilon};
        destination.Orientation = {
            source.orientation.x * inverseLength,
            source.orientation.y * inverseLength,
            source.orientation.z * inverseLength,
            source.orientation.w * inverseLength};
        return true;
    }
}


ViewFrustum MakeViewFrustum(
    const DirectX::XMFLOAT4X4& viewProjection) noexcept
{
    ViewFrustum frustum{};
    (void)TryMakeFrustumPlane(
        viewProjection._11 + viewProjection._14,
        viewProjection._21 + viewProjection._24,
        viewProjection._31 + viewProjection._34,
        viewProjection._41 + viewProjection._44,
        frustum.left);
    (void)TryMakeFrustumPlane(
        viewProjection._14 - viewProjection._11,
        viewProjection._24 - viewProjection._21,
        viewProjection._34 - viewProjection._31,
        viewProjection._44 - viewProjection._41,
        frustum.right);
    (void)TryMakeFrustumPlane(
        viewProjection._12 + viewProjection._14,
        viewProjection._22 + viewProjection._24,
        viewProjection._32 + viewProjection._34,
        viewProjection._42 + viewProjection._44,
        frustum.bottom);
    (void)TryMakeFrustumPlane(
        viewProjection._14 - viewProjection._12,
        viewProjection._24 - viewProjection._22,
        viewProjection._34 - viewProjection._32,
        viewProjection._44 - viewProjection._42,
        frustum.top);
    (void)TryMakeFrustumPlane(
        viewProjection._13,
        viewProjection._23,
        viewProjection._33,
        viewProjection._43,
        frustum.nearPlane);
    (void)TryMakeFrustumPlane(
        viewProjection._14 - viewProjection._13,
        viewProjection._24 - viewProjection._23,
        viewProjection._34 - viewProjection._33,
        viewProjection._44 - viewProjection._43,
        frustum.farPlane);
    return frustum;
}

Sphere3D TransformSphere(
    const Sphere3D& sphere,
    const DirectX::XMFLOAT4X4& world) noexcept
{
    if (!HasValidRadius(sphere.radius) ||
        !HasFiniteCoordinates(sphere.center) ||
        !std::isfinite(world._41) ||
        !std::isfinite(world._42) ||
        !std::isfinite(world._43))
    {
        return {{}, -1.0F};
    }

    const float worldScale = MaximumWorldScale(world);
    if (!std::isfinite(worldScale))
    {
        return {{}, -1.0F};
    }

    XMFLOAT3 center{};
    XMStoreFloat3(
        &center,
        XMVector3TransformCoord(
            XMLoadFloat3(&sphere.center),
            XMLoadFloat4x4(&world)));
    if (!HasFiniteCoordinates(center))
    {
        return {{}, -1.0F};
    }
    return {center, sphere.radius * worldScale};
}

VolumeIntersection Classify(
    const ViewFrustum& frustum,
    const Sphere3D& sphere,
    const float epsilon) noexcept
{
    if (!HasValidRadius(sphere.radius) ||
        !HasFiniteCoordinates(sphere.center))
    {
        return VolumeIntersection::Outside;
    }

    const float absoluteEpsilon = AbsEpsilon(epsilon);
    const std::array<const Plane3D*, 6> planes{
        &frustum.left,
        &frustum.right,
        &frustum.bottom,
        &frustum.top,
        &frustum.nearPlane,
        &frustum.farPlane};
    VolumeIntersection result = VolumeIntersection::Inside;
    for (const Plane3D* plane : planes)
    {
        const float normalLengthSquared = LengthSquared(plane->normal);
        if (!std::isfinite(normalLengthSquared) ||
            !std::isfinite(plane->distanceFromOrigin) ||
            normalLengthSquared <=
                std::numeric_limits<float>::epsilon())
        {
            return VolumeIntersection::Outside;
        }

        const float signedDistance =
            (Dot(plane->normal, sphere.center) -
                plane->distanceFromOrigin) /
            std::sqrt(normalLengthSquared);
        if (signedDistance < -sphere.radius - absoluteEpsilon)
        {
            return VolumeIntersection::Outside;
        }
        if (signedDistance <= sphere.radius + absoluteEpsilon)
        {
            result = VolumeIntersection::Intersecting;
        }
    }
    return result;
}

DirectX::XMFLOAT2 ClosestPoint(
    const LineSegment2D& segment,
    const DirectX::XMFLOAT2& point) noexcept
{
    const XMFLOAT2 direction = Subtract(segment.end, segment.start);
    const float lengthSquared = LengthSquared(direction);
    if (lengthSquared <= std::numeric_limits<float>::epsilon())
    {
        return segment.start;
    }

    const float parameter = std::clamp(
        Dot(Subtract(point, segment.start), direction) / lengthSquared,
        0.0F,
        1.0F);
    return {
        segment.start.x + direction.x * parameter,
        segment.start.y + direction.y * parameter};
}

DirectX::XMFLOAT2 ClosestPoint(
    const Obb2D& box,
    const DirectX::XMFLOAT2& point) noexcept
{
    if (!HasValidExtents(box.halfExtents))
    {
        return box.center;
    }

    const Axes2D axes = BoxAxes(box);
    const XMFLOAT2 relative = Subtract(point, box.center);
    const float localX = std::clamp(
        Dot(relative, axes.x),
        -box.halfExtents.x,
        box.halfExtents.x);
    const float localY = std::clamp(
        Dot(relative, axes.y),
        -box.halfExtents.y,
        box.halfExtents.y);
    return {
        box.center.x + axes.x.x * localX + axes.y.x * localY,
        box.center.y + axes.x.y * localX + axes.y.y * localY};
}

bool Intersects(
    const Circle2D& circle,
    const Line2D& line,
    const float epsilon) noexcept
{
    if (!HasValidRadius(circle.radius))
    {
        return false;
    }

    const float absoluteEpsilon = AbsEpsilon(epsilon);
    const float directionLengthSquared = LengthSquared(line.direction);
    if (!std::isfinite(directionLengthSquared) ||
        directionLengthSquared <= absoluteEpsilon * absoluteEpsilon)
    {
        return false;
    }

    const XMFLOAT2 relative = Subtract(circle.center, line.point);
    const float cross =
        relative.x * line.direction.y -
        relative.y * line.direction.x;
    const float safeDirectionLengthSquared = std::max(
        directionLengthSquared,
        std::numeric_limits<float>::min());
    const float distanceSquared =
        (cross * cross) / safeDirectionLengthSquared;
    const float allowedRadius = circle.radius + absoluteEpsilon;
    return distanceSquared <= allowedRadius * allowedRadius;
}

bool Intersects(
    const Circle2D& circle,
    const LineSegment2D& segment,
    const float epsilon) noexcept
{
    if (!HasValidRadius(circle.radius))
    {
        return false;
    }

    const XMFLOAT2 closest = ClosestPoint(segment, circle.center);
    const XMFLOAT2 delta = Subtract(closest, circle.center);
    const float allowedRadius = circle.radius + AbsEpsilon(epsilon);
    return LengthSquared(delta) <= allowedRadius * allowedRadius;
}

bool Intersects(
    const Circle2D& circle,
    const Obb2D& box,
    const float epsilon) noexcept
{
    if (!HasValidRadius(circle.radius) ||
        !HasValidExtents(box.halfExtents))
    {
        return false;
    }

    const XMFLOAT2 closest = ClosestPoint(box, circle.center);
    const XMFLOAT2 delta = Subtract(closest, circle.center);
    const float allowedRadius = circle.radius + AbsEpsilon(epsilon);
    return LengthSquared(delta) <= allowedRadius * allowedRadius;
}

bool Intersects(
    const Obb2D& first,
    const Obb2D& second,
    const float epsilon) noexcept
{
    if (!HasValidExtents(first.halfExtents) ||
        !HasValidExtents(second.halfExtents))
    {
        return false;
    }

    const float absoluteEpsilon = AbsEpsilon(epsilon);
    const Axes2D firstAxes = BoxAxes(first);
    const Axes2D secondAxes = BoxAxes(second);
    const std::array<XMFLOAT2, 4> separatingAxes{
        firstAxes.x,
        firstAxes.y,
        secondAxes.x,
        secondAxes.y};

    for (const XMFLOAT2& axis : separatingAxes)
    {
        if (IsSeparatedOnAxis(
                first,
                firstAxes,
                second,
                secondAxes,
                axis,
                absoluteEpsilon))
        {
            return false;
        }
    }
    return true;
}

bool Intersects(
    const Line2D& line,
    const Circle2D& circle,
    const float epsilon) noexcept
{
    return Intersects(circle, line, epsilon);
}

bool Intersects(
    const LineSegment2D& segment,
    const Circle2D& circle,
    const float epsilon) noexcept
{
    return Intersects(circle, segment, epsilon);
}

bool Intersects(
    const Obb2D& box,
    const Circle2D& circle,
    const float epsilon) noexcept
{
    return Intersects(circle, box, epsilon);
}

std::optional<LineHit3D> Intersect(
    const Plane3D& plane,
    const Line3D& line,
    const float epsilon) noexcept
{
    return IntersectPlane(
        plane,
        line.point,
        line.direction,
        -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        false,
        epsilon);
}

std::optional<LineHit3D> Intersect(
    const Plane3D& plane,
    const Ray3D& ray,
    const float epsilon) noexcept
{
    return IntersectPlane(
        plane,
        ray.origin,
        ray.direction,
        0.0F,
        std::numeric_limits<float>::infinity(),
        false,
        epsilon);
}

std::optional<LineHit3D> Intersect(
    const Plane3D& plane,
    const LineSegment3D& segment,
    const float epsilon) noexcept
{
    return IntersectPlane(
        plane,
        segment.start,
        Subtract(segment.end, segment.start),
        0.0F,
        1.0F,
        true,
        epsilon);
}

bool Intersects(
    const Plane3D& plane,
    const Line3D& line,
    const float epsilon) noexcept
{
    return Intersect(plane, line, epsilon).has_value();
}

bool Intersects(
    const Plane3D& plane,
    const Ray3D& ray,
    const float epsilon) noexcept
{
    return Intersect(plane, ray, epsilon).has_value();
}

bool Intersects(
    const Plane3D& plane,
    const LineSegment3D& segment,
    const float epsilon) noexcept
{
    return Intersect(plane, segment, epsilon).has_value();
}

std::optional<TriangleHit3D> Intersect(
    const Triangle3D& triangle,
    const Ray3D& ray,
    const float epsilon) noexcept
{
    const float absoluteEpsilon = AbsEpsilon(epsilon);
    const XMFLOAT3 firstEdge = Subtract(triangle.second, triangle.first);
    const XMFLOAT3 secondEdge = Subtract(triangle.third, triangle.first);
    const XMFLOAT3 directionCross = Cross(ray.direction, secondEdge);
    const float determinant = Dot(firstEdge, directionCross);
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= absoluteEpsilon)
    {
        return std::nullopt;
    }

    const float inverseDeterminant = 1.0F / determinant;
    const XMFLOAT3 originDelta = Subtract(ray.origin, triangle.first);
    const float secondWeight =
        Dot(originDelta, directionCross) * inverseDeterminant;
    if (secondWeight < -absoluteEpsilon ||
        secondWeight > 1.0F + absoluteEpsilon)
    {
        return std::nullopt;
    }

    const XMFLOAT3 deltaCross = Cross(originDelta, firstEdge);
    const float thirdWeight =
        Dot(ray.direction, deltaCross) * inverseDeterminant;
    if (thirdWeight < -absoluteEpsilon ||
        secondWeight + thirdWeight > 1.0F + absoluteEpsilon)
    {
        return std::nullopt;
    }

    float parameter = Dot(secondEdge, deltaCross) * inverseDeterminant;
    if (!std::isfinite(parameter) || parameter < -absoluteEpsilon)
    {
        return std::nullopt;
    }
    parameter = std::max(parameter, 0.0F);

    const XMFLOAT3 unnormalizedNormal = Cross(firstEdge, secondEdge);
    const float normalLengthSquared = LengthSquared(unnormalizedNormal);
    if (!std::isfinite(normalLengthSquared) ||
        normalLengthSquared <= absoluteEpsilon * absoluteEpsilon)
    {
        return std::nullopt;
    }
    const float inverseNormalLength = 1.0F / std::sqrt(normalLengthSquared);
    const float firstWeight = 1.0F - secondWeight - thirdWeight;
    return TriangleHit3D{
        AddScaled(ray.origin, ray.direction, parameter),
        {unnormalizedNormal.x * inverseNormalLength,
            unnormalizedNormal.y * inverseNormalLength,
            unnormalizedNormal.z * inverseNormalLength},
        {firstWeight, secondWeight, thirdWeight},
        parameter};
}

bool Intersects(
    const Triangle3D& triangle,
    const Ray3D& ray,
    const float epsilon) noexcept
{
    return Intersect(triangle, ray, epsilon).has_value();
}

bool Intersects(
    const Sphere3D& sphere,
    const Line3D& line,
    const float epsilon) noexcept
{
    return SphereIntersectsDirection(
        sphere,
        line.point,
        line.direction,
        -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        false,
        epsilon);
}

bool Intersects(
    const Sphere3D& sphere,
    const Ray3D& ray,
    const float epsilon) noexcept
{
    return SphereIntersectsDirection(
        sphere,
        ray.origin,
        ray.direction,
        0.0F,
        std::numeric_limits<float>::infinity(),
        false,
        epsilon);
}

bool Intersects(
    const Sphere3D& sphere,
    const LineSegment3D& segment,
    const float epsilon) noexcept
{
    return SphereIntersectsDirection(
        sphere,
        segment.start,
        Subtract(segment.end, segment.start),
        0.0F,
        1.0F,
        true,
        epsilon);
}

bool Intersects(
    const Sphere3D& sphere,
    const Obb3D& box,
    const float epsilon) noexcept
{
    if (!HasValidRadius(sphere.radius))
    {
        return false;
    }

    DirectX::BoundingOrientedBox directXBox;
    if (!TryMakeBox(box, directXBox, epsilon))
    {
        return false;
    }

    DirectX::BoundingSphere directXSphere;
    directXSphere.Center = sphere.center;
    directXSphere.Radius = sphere.radius + AbsEpsilon(epsilon);
    return directXSphere.Intersects(directXBox);
}

bool Intersects(
    const Obb3D& first,
    const Obb3D& second,
    const float epsilon) noexcept
{
    DirectX::BoundingOrientedBox firstBox;
    DirectX::BoundingOrientedBox secondBox;
    if (!TryMakeBox(first, firstBox, epsilon) ||
        !TryMakeBox(second, secondBox, epsilon))
    {
        return false;
    }
    return firstBox.Intersects(secondBox);
}

bool Intersects(
    const Line3D& line,
    const Sphere3D& sphere,
    const float epsilon) noexcept
{
    return Intersects(sphere, line, epsilon);
}

bool Intersects(
    const Ray3D& ray,
    const Sphere3D& sphere,
    const float epsilon) noexcept
{
    return Intersects(sphere, ray, epsilon);
}

bool Intersects(
    const LineSegment3D& segment,
    const Sphere3D& sphere,
    const float epsilon) noexcept
{
    return Intersects(sphere, segment, epsilon);
}

bool Intersects(
    const Obb3D& box,
    const Sphere3D& sphere,
    const float epsilon) noexcept
{
    return Intersects(sphere, box, epsilon);
}

} // namespace PhantomCore
