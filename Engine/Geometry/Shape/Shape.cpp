#include "Shape/Shape.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace PhantomCore
{
Shape::~Shape() = default;

std::span<const VertexAttributes> Shape::Vertices() const noexcept
{
    return vertices_;
}

std::span<const std::uint32_t> Shape::Indices() const noexcept
{
    return indices_;
}

std::size_t Shape::VertexCount() const noexcept
{
    return vertices_.size();
}

std::size_t Shape::IndexCount() const noexcept
{
    return indices_.size();
}

void Shape::SetMeshData(
    std::vector<VertexAttributes> vertices,
    std::vector<std::uint32_t> indices)
{
    if (vertices.empty() || indices.empty())
    {
        throw std::invalid_argument(
            "Shape mesh data must contain vertices and indices.");
    }

    const bool hasInvalidIndex = std::ranges::any_of(
        indices,
        [&vertices] (const std::uint32_t index)
        {
            return index >= vertices.size();
        });
    if (hasInvalidIndex)
    {
        throw std::out_of_range(
            "Shape mesh data contains an out-of-range index.");
    }

    vertices_ = std::move(vertices);
    indices_ = std::move(indices);
}

} // namespace PhantomCore
