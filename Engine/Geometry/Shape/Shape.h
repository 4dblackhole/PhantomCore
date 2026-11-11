#pragma once

#include "Shape/Vertex.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace PhantomCore
{
class Shape
{
public:
    virtual ~Shape();

    [[nodiscard]] std::span<const VertexAttributes> Vertices()
        const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> Indices()
        const noexcept;
    [[nodiscard]] std::size_t VertexCount() const noexcept;
    [[nodiscard]] std::size_t IndexCount() const noexcept;

    // Selects the final GPU vertex layout at compile time while preserving
    // the Shape's canonical attributes.
    template <ShapeVertex VertexType>
    [[nodiscard]] std::vector<VertexType> CreateVertices() const
    {
        std::vector<VertexType> result;
        result.reserve(vertices_.size());
        for (const VertexAttributes& attributes : vertices_)
        {
            result.push_back(VertexType::FromAttributes(attributes));
        }
        return result;
    }

protected:
    Shape() = default;
    Shape(const Shape&) = default;
    Shape& operator=(const Shape&) = default;
    Shape(Shape&&) noexcept = default;
    Shape& operator=(Shape&&) noexcept = default;

    void SetMeshData(
        std::vector<VertexAttributes> vertices,
        std::vector<std::uint32_t> indices);

private:
    std::vector<VertexAttributes> vertices_;
    std::vector<std::uint32_t> indices_;
};

} // namespace PhantomCore
