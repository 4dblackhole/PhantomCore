#include "Primitive/CubeShape.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace PhantomCore
{
namespace
{
    Float4 CornerColor(
        const Float3& position) noexcept
    {
        if (position.z < 0.0F)
        {
            if (position.x < 0.0F && position.y < 0.0F)
            {
                return {1.0F, 0.0F, 0.0F, 1.0F};
            }
            if (position.x < 0.0F)
            {
                return {0.0F, 1.0F, 0.0F, 1.0F};
            }
            if (position.y > 0.0F)
            {
                return {0.0F, 0.0F, 1.0F, 1.0F};
            }
            return {0.0F, 1.0F, 1.0F, 1.0F};
        }

        if (position.x < 0.0F && position.y < 0.0F)
        {
            return {1.0F, 1.0F, 0.0F, 1.0F};
        }
        if (position.x < 0.0F)
        {
            return {1.0F, 0.0F, 1.0F, 1.0F};
        }
        if (position.y > 0.0F)
        {
            return {1.0F, 1.0F, 1.0F, 1.0F};
        }
        return {0.0F, 0.0F, 0.0F, 1.0F};
    }

    void AppendFace(
        std::vector<VertexAttributes>& vertices,
        std::vector<std::uint32_t>& indices,
        const std::array<Float3, 4>& positions,
        const Float3& normal)
    {
        constexpr std::array uvs{
            Float2{0.0F, 1.0F},
            Float2{0.0F, 0.0F},
            Float2{1.0F, 0.0F},
            Float2{1.0F, 1.0F}};

        const auto baseIndex =
            static_cast<std::uint32_t>(vertices.size());
        for (std::size_t index = 0; index < positions.size(); ++index)
        {
            vertices.push_back(VertexAttributes{
                positions[index],
                uvs[index],
                normal,
                CornerColor(positions[index])});
        }

        indices.insert(
            indices.end(),
            {
                baseIndex,
                baseIndex + 1,
                baseIndex + 2,
                baseIndex,
                baseIndex + 2,
                baseIndex + 3});
    }
}

CubeShape::CubeShape(const float sideLength)
{
    if (sideLength <= 0.0F)
    {
        throw std::invalid_argument(
            "CubeShape side length must be positive.");
    }

    const float half = sideLength * 0.5F;
    std::vector<VertexAttributes> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(24);
    indices.reserve(36);

    AppendFace(
        vertices,
        indices,
        {{{-half, -half, -half},
          {-half, half, -half},
          {half, half, -half},
          {half, -half, -half}}},
        {0.0F, 0.0F, -1.0F});
    AppendFace(
        vertices,
        indices,
        {{{-half, -half, half},
          {half, -half, half},
          {half, half, half},
          {-half, half, half}}},
        {0.0F, 0.0F, 1.0F});
    AppendFace(
        vertices,
        indices,
        {{{-half, -half, half},
          {-half, half, half},
          {-half, half, -half},
          {-half, -half, -half}}},
        {-1.0F, 0.0F, 0.0F});
    AppendFace(
        vertices,
        indices,
        {{{half, -half, -half},
          {half, half, -half},
          {half, half, half},
          {half, -half, half}}},
        {1.0F, 0.0F, 0.0F});
    AppendFace(
        vertices,
        indices,
        {{{-half, half, -half},
          {-half, half, half},
          {half, half, half},
          {half, half, -half}}},
        {0.0F, 1.0F, 0.0F});
    AppendFace(
        vertices,
        indices,
        {{{-half, -half, half},
          {-half, -half, -half},
          {half, -half, -half},
          {half, -half, half}}},
        {0.0F, -1.0F, 0.0F});

    SetMeshData(std::move(vertices), std::move(indices));
}

} // namespace PhantomCore
