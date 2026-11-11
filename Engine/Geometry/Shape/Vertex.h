#pragma once

// Shape feature: backend-neutral canonical and selectable vertex layouts.

#include <DirectXMath.h>

#include <concepts>
#include <type_traits>

// Public canonical attributes retained by Shape regardless of the GPU vertex
// layout selected by a Client.
namespace PhantomCore
{
struct VertexAttributes final
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT2 uv{};
    DirectX::XMFLOAT3 normal{};
    DirectX::XMFLOAT4 color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct VertexPosition final
{
    DirectX::XMFLOAT3 position{};

    [[nodiscard]] static VertexPosition FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {attributes.position};
    }
};

struct VertexPositionUv final
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT2 uv{};

    [[nodiscard]] static VertexPositionUv FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {attributes.position, attributes.uv};
    }
};

struct VertexPositionColor final
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT4 color{};

    [[nodiscard]] static VertexPositionColor FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {attributes.position, attributes.color};
    }
};

struct VertexPositionUvColor final
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT2 uv{};
    DirectX::XMFLOAT4 color{};

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
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{};

    [[nodiscard]] static VertexPositionNormal FromAttributes(
        const VertexAttributes& attributes) noexcept
    {
        return {attributes.position, attributes.normal};
    }
};

struct VertexPositionNormalUv final
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{};
    DirectX::XMFLOAT2 uv{};

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
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{};
    DirectX::XMFLOAT2 uv{};
    DirectX::XMFLOAT4 color{};

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

static_assert(ShapeVertex<VertexPosition>);
static_assert(ShapeVertex<VertexPositionUv>);
static_assert(ShapeVertex<VertexPositionColor>);
static_assert(ShapeVertex<VertexPositionUvColor>);
static_assert(ShapeVertex<VertexPositionNormal>);
static_assert(ShapeVertex<VertexPositionNormalUv>);
static_assert(ShapeVertex<VertexPositionNormalUvColor>);

} // namespace PhantomCore
