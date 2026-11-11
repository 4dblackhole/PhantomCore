#include "Primitive/CurvedRectangleShape.h"

#include <DirectXMath.h>

#include <cmath>
#include <stdexcept>
#include <vector>


namespace PhantomCore
{
CurvedRectangleShape::CurvedRectangleShape(
    const float width,
    const float height,
    const float curvatureRadians,
    const std::uint32_t horizontalSegments)
{
    if (width <= 0.0F || height <= 0.0F)
    {
        throw std::invalid_argument(
            "CurvedRectangleShape width and height must be positive.");
    }
    if (curvatureRadians <= 0.0F || curvatureRadians >= DirectX::XM_PI)
    {
        throw std::invalid_argument(
            "CurvedRectangleShape curvature must be between zero and pi.");
    }
    if (horizontalSegments == 0)
    {
        throw std::invalid_argument(
            "CurvedRectangleShape requires at least one segment.");
    }

    const float radius = width / curvatureRadians;
    const float halfHeight = height * 0.5F;
    const float halfCurvature = curvatureRadians * 0.5F;

    std::vector<VertexAttributes> vertices;
    vertices.reserve(
        (static_cast<std::size_t>(horizontalSegments) + 1U) * 2U);

    for (std::uint32_t column = 0; column <= horizontalSegments; ++column)
    {
        const float u = static_cast<float>(column) /
            static_cast<float>(horizontalSegments);
        const float angle = -halfCurvature + curvatureRadians * u;
        const float sine = std::sin(angle);
        const float cosine = std::cos(angle);
        const float x = radius * sine;
        const float z = radius * (1.0F - cosine);
        const DirectX::XMFLOAT3 normal{sine, 0.0F, -cosine};

        vertices.push_back({
            {x, -halfHeight, z},
            {u, 1.0F},
            normal});
        vertices.push_back({
            {x, halfHeight, z},
            {u, 0.0F},
            normal});
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(horizontalSegments) * 6U);
    for (std::uint32_t segment = 0;
         segment < horizontalSegments;
         ++segment)
    {
        const std::uint32_t bottomLeft = segment * 2U;
        const std::uint32_t topLeft = bottomLeft + 1U;
        const std::uint32_t bottomRight = bottomLeft + 2U;
        const std::uint32_t topRight = bottomLeft + 3U;
        indices.insert(indices.end(), {
            bottomLeft,
            topLeft,
            topRight,
            bottomLeft,
            topRight,
            bottomRight});
    }

    SetMeshData(std::move(vertices), std::move(indices));
}

} // namespace PhantomCore
