#pragma once

// Shape feature: backend-neutral canonical and selectable vertex layouts.

#include "../../Math/Math.h"

#include <concepts>
#include <type_traits>

// Public canonical attributes retained by Shape regardless of the GPU vertex
// layout selected by a Client.
namespace PhantomCore
{
struct VertexAttributes final
{
    Float3 position{};
    Float2 uv{};
    Float3 normal{};
    Float4 color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct VertexPosition final
{
    Float3 position{};

    [[nodiscard]] static VertexPosition FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {attributes.position};
    }
};

struct VertexPositionUv final
{
    Float3 position{};
    Float2 uv{};

    [[nodiscard]] static VertexPositionUv FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {attributes.position, attributes.uv};
    }
};

struct VertexPositionColor final
{
    Float3 position{};
    Float4 color{};

    [[nodiscard]] static VertexPositionColor FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {attributes.position, attributes.color};
    }
};

struct VertexPositionUvColor final
{
    Float3 position{};
    Float2 uv{};
    Float4 color{};

    [[nodiscard]] static VertexPositionUvColor FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {
            attributes.position,
            attributes.uv,
            attributes.color};
    }
};

struct VertexPositionNormal final
{
    Float3 position{};
    Float3 normal{};

    [[nodiscard]] static VertexPositionNormal FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {attributes.position, attributes.normal};
    }
};

struct VertexPositionNormalUv final
{
    Float3 position{};
    Float3 normal{};
    Float2 uv{};

    [[nodiscard]] static VertexPositionNormalUv FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {
            attributes.position,
            attributes.normal,
            attributes.uv};
    }
};

struct VertexPositionNormalUvColor final
{
    Float3 position{};
    Float3 normal{};
    Float2 uv{};
    Float4 color{};

    [[nodiscard]] static VertexPositionNormalUvColor FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {
            attributes.position,
            attributes.normal,
            attributes.uv,
            attributes.color};
    }
};

// A custom vertex type can participate by remaining standard-layout and
// trivially copyable, and by providing the same FromAttributes function.
template <typename VertexType>
concept ShapeVertex =
    std::is_standard_layout_v<VertexType> &&
    std::is_trivially_copyable_v<VertexType> &&
    requires (const VertexAttributes& attributes)
    {
        {
            VertexType::FromAttributes(attributes)
        } noexcept -> std::same_as<VertexType>;
    };

// Keep shader input strides and offsets unchanged across the storage migration.
static_assert(sizeof(VertexAttributes) == 48 && offsetof(VertexAttributes, color) == 32);
static_assert(sizeof(VertexPosition) == 12);
static_assert(sizeof(VertexPositionUv) == 20 && offsetof(VertexPositionUv, uv) == 12);
static_assert(sizeof(VertexPositionColor) == 28 && offsetof(VertexPositionColor, color) == 12);
static_assert(sizeof(VertexPositionUvColor) == 36 && offsetof(VertexPositionUvColor, color) == 20);
static_assert(sizeof(VertexPositionNormal) == 24 && offsetof(VertexPositionNormal, normal) == 12);
static_assert(sizeof(VertexPositionNormalUv) == 32 && offsetof(VertexPositionNormalUv, uv) == 24);
static_assert(sizeof(VertexPositionNormalUvColor) == 48 && offsetof(VertexPositionNormalUvColor, color) == 32);

static_assert(ShapeVertex<VertexPosition>);
static_assert(ShapeVertex<VertexPositionUv>);
static_assert(ShapeVertex<VertexPositionColor>);
static_assert(ShapeVertex<VertexPositionUvColor>);
static_assert(ShapeVertex<VertexPositionNormal>);
static_assert(ShapeVertex<VertexPositionNormalUv>);
static_assert(ShapeVertex<VertexPositionNormalUvColor>);

} // namespace PhantomCore
