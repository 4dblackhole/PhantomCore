#pragma once

#include <cstddef>
#include <type_traits>

namespace PhantomCore
{
// Plain float storage. Matrices use row vectors and row-major storage.
struct Float2 { float x{}; float y{}; };
struct Float3 { float x{}; float y{}; float z{}; };
struct Float4 { float x{}; float y{}; float z{}; float w{}; };
struct Float4x4 { float m[4][4]{}; };

static_assert(std::is_standard_layout_v<Float2> && std::is_trivially_copyable_v<Float2>);
static_assert(std::is_standard_layout_v<Float3> && std::is_trivially_copyable_v<Float3>);
static_assert(std::is_standard_layout_v<Float4> && std::is_trivially_copyable_v<Float4>);
static_assert(std::is_standard_layout_v<Float4x4> && std::is_trivially_copyable_v<Float4x4>);
static_assert(sizeof(Float2) == 8 && sizeof(Float3) == 12 && sizeof(Float4) == 16);
static_assert(sizeof(Float4x4) == 64 && offsetof(Float3, z) == 8 && offsetof(Float4, w) == 12);

inline constexpr float Pi = 3.14159265358979323846F;
inline constexpr float HalfPi = Pi / 2.0F;
inline constexpr float QuarterPi = Pi / 4.0F;

[[nodiscard]] Float4x4 IdentityMatrix() noexcept;
[[nodiscard]] Float4x4 TranslationMatrix(float x, float y, float z) noexcept;
[[nodiscard]] Float4x4 ScalingMatrix(float x, float y, float z) noexcept;
[[nodiscard]] Float4x4 RotationYMatrix(float radians) noexcept;
[[nodiscard]] Float4x4 OrthographicLHMatrix(float width, float height, float nearPlane, float farPlane) noexcept;
[[nodiscard]] Float4x4 Multiply(const Float4x4& left, const Float4x4& right) noexcept;
// Singular matrices follow IEEE floating-point behavior; callers validate invertibility when required.
[[nodiscard]] Float4x4 Inverse(const Float4x4& matrix) noexcept;
[[nodiscard]] Float3 TransformPoint(const Float3& point, const Float4x4& matrix) noexcept;
[[nodiscard]] Float3 TransformDirection(const Float3& direction, const Float4x4& matrix) noexcept;
} // namespace PhantomCore
