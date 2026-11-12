#pragma once

// Backend-only conversion. Public storage never aliases DirectX objects.
#include "Math.h"
#include <DirectXMath.h>
#include <bit>

namespace PhantomCore::NativeMath
{
static_assert(sizeof(Float2) == sizeof(DirectX::XMFLOAT2));
static_assert(alignof(Float2) == alignof(DirectX::XMFLOAT2));
[[nodiscard]] inline DirectX::XMFLOAT2 ToNative(const Float2& value) noexcept
{ return std::bit_cast<DirectX::XMFLOAT2>(value); }
[[nodiscard]] inline DirectX::XMVECTOR LoadFloat2(const Float2* value) noexcept
{
    const auto native = ToNative(*value);
    return DirectX::XMLoadFloat2(&native);
}
inline void StoreFloat2(Float2* destination, DirectX::FXMVECTOR value) noexcept
{
    DirectX::XMFLOAT2 native;
    DirectX::XMStoreFloat2(&native, value);
    *destination = std::bit_cast<Float2>(native);
}
static_assert(sizeof(Float3) == sizeof(DirectX::XMFLOAT3));
static_assert(alignof(Float3) == alignof(DirectX::XMFLOAT3));
[[nodiscard]] inline DirectX::XMFLOAT3 ToNative(const Float3& value) noexcept
{ return std::bit_cast<DirectX::XMFLOAT3>(value); }
[[nodiscard]] inline DirectX::XMVECTOR LoadFloat3(const Float3* value) noexcept
{
    const auto native = ToNative(*value);
    return DirectX::XMLoadFloat3(&native);
}
inline void StoreFloat3(Float3* destination, DirectX::FXMVECTOR value) noexcept
{
    DirectX::XMFLOAT3 native;
    DirectX::XMStoreFloat3(&native, value);
    *destination = std::bit_cast<Float3>(native);
}
static_assert(sizeof(Float4) == sizeof(DirectX::XMFLOAT4));
static_assert(alignof(Float4) == alignof(DirectX::XMFLOAT4));
[[nodiscard]] inline DirectX::XMFLOAT4 ToNative(const Float4& value) noexcept
{ return std::bit_cast<DirectX::XMFLOAT4>(value); }
[[nodiscard]] inline DirectX::XMVECTOR LoadFloat4(const Float4* value) noexcept
{
    const auto native = ToNative(*value);
    return DirectX::XMLoadFloat4(&native);
}
inline void StoreFloat4(Float4* destination, DirectX::FXMVECTOR value) noexcept
{
    DirectX::XMFLOAT4 native;
    DirectX::XMStoreFloat4(&native, value);
    *destination = std::bit_cast<Float4>(native);
}
static_assert(sizeof(Float4x4) == sizeof(DirectX::XMFLOAT4X4));
static_assert(alignof(Float4x4) == alignof(DirectX::XMFLOAT4X4));
[[nodiscard]] inline DirectX::XMFLOAT4X4 ToNative(const Float4x4& value) noexcept
{ return std::bit_cast<DirectX::XMFLOAT4X4>(value); }
[[nodiscard]] inline DirectX::XMMATRIX LoadFloat4x4(const Float4x4* value) noexcept
{
    const auto native = ToNative(*value);
    return DirectX::XMLoadFloat4x4(&native);
}
inline void StoreFloat4x4(Float4x4* destination, DirectX::FXMMATRIX value) noexcept
{
    DirectX::XMFLOAT4X4 native;
    DirectX::XMStoreFloat4x4(&native, value);
    *destination = std::bit_cast<Float4x4>(native);
}
} // namespace PhantomCore::NativeMath
