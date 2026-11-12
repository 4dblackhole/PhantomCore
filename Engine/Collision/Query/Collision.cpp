#include "Query/Collision.h"
#include "../../Math/DirectXMathBridge.h"

#include <DirectXCollision.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace PhantomCore
{
namespace
{

    [[nodiscard]] float AbsEpsilon(const float epsilon) noexcept
    {
        return std::isfinite(epsilon)
            ? std::abs(epsilon)
            : DefaultEpsilon;
    }

    [[nodiscard]] float Dot(
        const Float2& first,
        const Float2& second) noexcept
    {
        return first.x * second.x + first.y * second.y;
    }

    [[nodiscard]] float Dot(
        const Float3& first,
        const Float3& second) noexcept
    {
        return first.x * second.x +
            first.y * second.y +
            first.z * second.z;
    }

    [[nodiscard]] Float2 Subtract(
        const Float2& first,
        const Float2& second) noexcept
    {
        return {first.x - second.x, first.y - second.y};
    }

    [[nodiscard]] Float3 Subtract(
        const Float3& first,
        const Float3& second) noexcept
    {
        return {
            first.x - second.x,
            first.y - second.y,
            first.z - second.z};
    }

    [[nodiscard]] Float3 AddScaled(
        const Float3& point,
        const Float3& direction,
        const float parameter) noexcept
    {
        return {
            point.x + direction.x * parameter,
            point.y + direction.y * parameter,
            point.z + direction.z * parameter};
    }

    [[nodiscard]] Float3 Cross(
        const Float3& first,
        const Float3& second) noexcept
    {
        return {
            first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
    }

    [[nodiscard]] float LengthSquared(const Float2& value) noexcept
    {
        return Dot(value, value);
    }

    [[nodiscard]] float LengthSquared(const Float3& value) noexcept
    {
        return Dot(value, value);
    }

    [[nodiscard]] bool HasValidRadius(const float radius) noexcept
    {
        return std::isfinite(radius) && radius >= 0.0F;
    }

    [[nodiscard]] bool HasFiniteCoordinates(const Float3& value) noexcept
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
        const Float4x4& world) noexcept
    {
        const std::array<Float3, 3> axes{
            Float3{world.m[0][0], world.m[0][1], world.m[0][2]},
            Float3{world.m[1][0], world.m[1][1], world.m[1][2]},
            Float3{world.m[2][0], world.m[2][1], world.m[2][2]}};
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
            std::abs(world.m[0][0]) + std::abs(world.m[0][1]) + std::abs(world.m[0][2]),
            std::abs(world.m[1][0]) + std::abs(world.m[1][1]) + std::abs(world.m[1][2]),
            std::abs(world.m[2][0]) + std::abs(world.m[2][1]) + std::abs(world.m[2][2])});
        const float maximumColumnSum = std::max({
            std::abs(world.m[0][0]) + std::abs(world.m[1][0]) + std::abs(world.m[2][0]),
            std::abs(world.m[0][1]) + std::abs(world.m[1][1]) + std::abs(world.m[2][1]),
            std::abs(world.m[0][2]) + std::abs(world.m[1][2]) + std::abs(world.m[2][2])});
        return std::sqrt(maximumRowSum * maximumColumnSum);
    }

    [[nodiscard]] bool HasValidExtents(const Float2& extents) noexcept
    {
        return std::isfinite(extents.x) &&
            std::isfinite(extents.y) &&
            extents.x >= 0.0F &&
            extents.y >= 0.0F;
    }

    [[nodiscard]] bool HasValidExtents(const Float3& extents) noexcept
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
        Float2 x;
        Float2 y;
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
        const Float2& axis) noexcept
    {
        return box.halfExtents.x * std::abs(Dot(boxAxes.x, axis)) +
            box.halfExtents.y * std::abs(Dot(boxAxes.y, axis));
    }

    [[nodiscard]] bool IsSeparatedOnAxis(
        const Obb2D& first,
        const Axes2D& firstAxes,
        const Obb2D& second,
        const Axes2D& secondAxes,
        const Float2& axis,
        const float epsilon) noexcept
    {
        const Float2 centerDelta = Subtract(second.center, first.center);
        const float centerDistance = std::abs(Dot(centerDelta, axis));
        const float allowedDistance =
            ProjectionRadius(first, firstAxes, axis) +
            ProjectionRadius(second, secondAxes, axis) +
            epsilon;
        return centerDistance > allowedDistance;
    }

    [[nodiscard]] std::optional<LineHit3D> IntersectPlane(
        const Plane3D& plane,
        const Float3& origin,
        const Float3& direction,
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
                const Float3 normal{
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
                const Float3 normal{
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

        const Float3 normal{
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
        const Float3& origin,
        const Float3& direction,
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
            const Float3 centerDelta = Subtract(origin, sphere.center);
            const float allowedRadius = sphere.radius + absoluteEpsilon;
            return LengthSquared(centerDelta) <=
                allowedRadius * allowedRadius;
        }

        const Float3 toCenter = Subtract(sphere.center, origin);
        float parameter = Dot(toCenter, direction) / directionLengthSquared;
        parameter = std::clamp(
            parameter,
            minimumParameter,
            maximumParameter);
        const Float3 closest = AddScaled(origin, direction, parameter);
        const Float3 delta = Subtract(closest, sphere.center);
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
        destination.Center = NativeMath::ToNative(source.center);
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
    const Float4x4& viewProjection) noexcept
{
    ViewFrustum frustum{};
    (void)TryMakeFrustumPlane(
        viewProjection.m[0][0] + viewProjection.m[0][3],
        viewProjection.m[1][0] + viewProjection.m[1][3],
        viewProjection.m[2][0] + viewProjection.m[2][3],
        viewProjection.m[3][0] + viewProjection.m[3][3],
        frustum.left);
    (void)TryMakeFrustumPlane(
        viewProjection.m[0][3] - viewProjection.m[0][0],
        viewProjection.m[1][3] - viewProjection.m[1][0],
        viewProjection.m[2][3] - viewProjection.m[2][0],
        viewProjection.m[3][3] - viewProjection.m[3][0],
        frustum.right);
    (void)TryMakeFrustumPlane(
        viewProjection.m[0][1] + viewProjection.m[0][3],
        viewProjection.m[1][1] + viewProjection.m[1][3],
        viewProjection.m[2][1] + viewProjection.m[2][3],
        viewProjection.m[3][1] + viewProjection.m[3][3],
        frustum.bottom);
    (void)TryMakeFrustumPlane(
        viewProjection.m[0][3] - viewProjection.m[0][1],
        viewProjection.m[1][3] - viewProjection.m[1][1],
        viewProjection.m[2][3] - viewProjection.m[2][1],
        viewProjection.m[3][3] - viewProjection.m[3][1],
        frustum.top);
    (void)TryMakeFrustumPlane(
        viewProjection.m[0][2],
        viewProjection.m[1][2],
        viewProjection.m[2][2],
        viewProjection.m[3][2],
        frustum.nearPlane);
    (void)TryMakeFrustumPlane(
        viewProjection.m[0][3] - viewProjection.m[0][2],
        viewProjection.m[1][3] - viewProjection.m[1][2],
        viewProjection.m[2][3] - viewProjection.m[2][2],
        viewProjection.m[3][3] - viewProjection.m[3][2],
        frustum.farPlane);
    return frustum;
}

Sphere3D TransformSphere(
    const Sphere3D& sphere,
    const Float4x4& world) noexcept
{
    if (!HasValidRadius(sphere.radius) ||
        !HasFiniteCoordinates(sphere.center) ||
        !std::isfinite(world.m[3][0]) ||
        !std::isfinite(world.m[3][1]) ||
        !std::isfinite(world.m[3][2]))
    {
        return {{}, -1.0F};
    }

    const float worldScale = MaximumWorldScale(world);
    if (!std::isfinite(worldScale))
    {
        return {{}, -1.0F};
    }

    Float3 center{};
    NativeMath::StoreFloat3(
        &center,
        XMVector3TransformCoord(
            NativeMath::LoadFloat3(&sphere.center),
            NativeMath::LoadFloat4x4(&world)));
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

Float2 ClosestPoint(
    const LineSegment2D& segment,
    const Float2& point) noexcept
{
    const Float2 direction = Subtract(segment.end, segment.start);
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

Float2 ClosestPoint(
    const Obb2D& box,
    const Float2& point) noexcept
{
    if (!HasValidExtents(box.halfExtents))
    {
        return box.center;
    }

    const Axes2D axes = BoxAxes(box);
    const Float2 relative = Subtract(point, box.center);
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

    const Float2 relative = Subtract(circle.center, line.point);
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

    const Float2 closest = ClosestPoint(segment, circle.center);
    const Float2 delta = Subtract(closest, circle.center);
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

    const Float2 closest = ClosestPoint(box, circle.center);
    const Float2 delta = Subtract(closest, circle.center);
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
    const std::array<Float2, 4> separatingAxes{
        firstAxes.x,
        firstAxes.y,
        secondAxes.x,
        secondAxes.y};

    for (const Float2& axis : separatingAxes)
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
    const Float3 firstEdge = Subtract(triangle.second, triangle.first);
    const Float3 secondEdge = Subtract(triangle.third, triangle.first);
    const Float3 directionCross = Cross(ray.direction, secondEdge);
    const float determinant = Dot(firstEdge, directionCross);
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= absoluteEpsilon)
    {
        return std::nullopt;
    }

    const float inverseDeterminant = 1.0F / determinant;
    const Float3 originDelta = Subtract(ray.origin, triangle.first);
    const float secondWeight =
        Dot(originDelta, directionCross) * inverseDeterminant;
    if (secondWeight < -absoluteEpsilon ||
        secondWeight > 1.0F + absoluteEpsilon)
    {
        return std::nullopt;
    }

    const Float3 deltaCross = Cross(originDelta, firstEdge);
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

    const Float3 unnormalizedNormal = Cross(firstEdge, secondEdge);
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
    directXSphere.Center = NativeMath::ToNative(sphere.center);
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
