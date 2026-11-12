#include "Math.h"
#include "DirectXMathBridge.h"

namespace PhantomCore
{
namespace
{
    [[nodiscard]] Float4x4 StoreMatrix(DirectX::FXMMATRIX matrix) noexcept
    {
        Float4x4 result;
        NativeMath::StoreFloat4x4(&result, matrix);
        return result;
    }
}
Float4x4 IdentityMatrix() noexcept { return StoreMatrix(DirectX::XMMatrixIdentity()); }
Float4x4 TranslationMatrix(float x, float y, float z) noexcept { return StoreMatrix(DirectX::XMMatrixTranslation(x, y, z)); }
Float4x4 ScalingMatrix(float x, float y, float z) noexcept { return StoreMatrix(DirectX::XMMatrixScaling(x, y, z)); }
Float4x4 RotationYMatrix(float radians) noexcept { return StoreMatrix(DirectX::XMMatrixRotationY(radians)); }
Float4x4 OrthographicLHMatrix(float width, float height, float nearPlane, float farPlane) noexcept
{ return StoreMatrix(DirectX::XMMatrixOrthographicLH(width, height, nearPlane, farPlane)); }
Float4x4 Multiply(const Float4x4& left, const Float4x4& right) noexcept
{ return StoreMatrix(DirectX::XMMatrixMultiply(NativeMath::LoadFloat4x4(&left), NativeMath::LoadFloat4x4(&right))); }
Float4x4 Inverse(const Float4x4& matrix) noexcept
{ return StoreMatrix(DirectX::XMMatrixInverse(nullptr, NativeMath::LoadFloat4x4(&matrix))); }
Float3 TransformPoint(const Float3& point, const Float4x4& matrix) noexcept
{
    Float3 result;
    NativeMath::StoreFloat3(&result, DirectX::XMVector3TransformCoord(NativeMath::LoadFloat3(&point), NativeMath::LoadFloat4x4(&matrix)));
    return result;
}
Float3 TransformDirection(const Float3& direction, const Float4x4& matrix) noexcept
{
    Float3 result;
    NativeMath::StoreFloat3(&result, DirectX::XMVector3TransformNormal(NativeMath::LoadFloat3(&direction), NativeMath::LoadFloat4x4(&matrix)));
    return result;
}
} // namespace PhantomCore
