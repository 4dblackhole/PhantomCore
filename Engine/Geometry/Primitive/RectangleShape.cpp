#include "Primitive/RectangleShape.h"

#include <stdexcept>
#include <vector>

namespace PhantomCore
{
RectangleShape::RectangleShape(const float width, const float height)
{
    if (width <= 0.0F || height <= 0.0F)
    {
        throw std::invalid_argument(
            "RectangleShape width and height must be positive.");
    }

    const float halfWidth = width * 0.5F;
    const float halfHeight = height * 0.5F;
    constexpr Float3 normal{0.0F, 0.0F, -1.0F};

    std::vector<VertexAttributes> vertices{
        {{-halfWidth, -halfHeight, 0.0F}, {0.0F, 1.0F}, normal},
        {{-halfWidth, halfHeight, 0.0F}, {0.0F, 0.0F}, normal},
        {{halfWidth, halfHeight, 0.0F}, {1.0F, 0.0F}, normal},
        {{halfWidth, -halfHeight, 0.0F}, {1.0F, 1.0F}, normal}};

    std::vector<std::uint32_t> indices{0, 1, 2, 0, 2, 3};
    SetMeshData(std::move(vertices), std::move(indices));
}

} // namespace PhantomCore
