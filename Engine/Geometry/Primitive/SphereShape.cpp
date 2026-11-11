#include "Primitive/SphereShape.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace PhantomCore
{
SphereShape::SphereShape(
    const float radius,
    const std::uint32_t sliceCount,
    const std::uint32_t stackCount)
{
    // This constructor performs one mesh-generation task in three phases:
    // validate tessellation, generate latitude/longitude attributes, then
    // connect adjacent rings with indexed triangles.
    if (radius <= 0.0F)
    {
        throw std::invalid_argument(
            "SphereShape radius must be positive.");
    }
    if (sliceCount < 3 || stackCount < 2)
    {
        throw std::invalid_argument(
            "SphereShape requires at least 3 slices and 2 stacks.");
    }

    const std::uint64_t rowVertexCount =
        static_cast<std::uint64_t>(sliceCount) + 1;
    const std::uint64_t totalVertexCount =
        (static_cast<std::uint64_t>(stackCount) + 1) *
        rowVertexCount;
    if (totalVertexCount >
        std::numeric_limits<std::uint32_t>::max())
    {
        throw std::length_error(
            "SphereShape contains too many vertices for 32-bit indices.");
    }

    std::vector<VertexAttributes> vertices;
    vertices.reserve(static_cast<std::size_t>(totalVertexCount));

    for (std::uint32_t stack = 0; stack <= stackCount; ++stack)
    {
        const float v =
            static_cast<float>(stack) /
            static_cast<float>(stackCount);
        const float phi = std::numbers::pi_v<float> * v;
        const float y = std::cos(phi);
        const float ringRadius = std::sin(phi);

        for (std::uint32_t slice = 0; slice <= sliceCount; ++slice)
        {
            const float u =
                static_cast<float>(slice) /
                static_cast<float>(sliceCount);
            const float theta =
                2.0F * std::numbers::pi_v<float> * u;
            const float x = ringRadius * std::sin(theta);
            const float z = ringRadius * std::cos(theta);

            vertices.push_back(VertexAttributes{
                {radius * x, radius * y, radius * z},
                {u, v},
                {x, y, z},
                {1.0F, 1.0F, 1.0F, 1.0F}});
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(
        static_cast<std::size_t>(sliceCount) *
        static_cast<std::size_t>(stackCount - 1) * 6);

    const std::uint32_t rowSize = sliceCount + 1;
    for (std::uint32_t stack = 0; stack < stackCount; ++stack)
    {
        for (std::uint32_t slice = 0; slice < sliceCount; ++slice)
        {
            const std::uint32_t upperLeft =
                stack * rowSize + slice;
            const std::uint32_t lowerLeft = upperLeft + rowSize;
            const std::uint32_t upperRight = upperLeft + 1;
            const std::uint32_t lowerRight = lowerLeft + 1;

            if (stack != 0)
            {
                indices.insert(
                    indices.end(),
                    {upperLeft, lowerLeft, upperRight});
            }
            if (stack + 1 != stackCount)
            {
                indices.insert(
                    indices.end(),
                    {upperRight, lowerLeft, lowerRight});
            }
        }
    }

    SetMeshData(std::move(vertices), std::move(indices));
}

} // namespace PhantomCore
