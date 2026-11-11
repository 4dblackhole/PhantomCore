#pragma once

// System feature: backend-neutral view and projection matrices.

#include "Query/Collision.h"

#include <DirectXMath.h>

#include <cstdint>


namespace PhantomCore
{
enum class CameraProjectionType : std::uint8_t
{
    Perspective,
    Orthographic,
};

// Backend-neutral, left-handed camera state for 3D and 2D scenes.
// Position/yaw/pitch produce the view matrix.  Select Perspective for a
// FOV-based 3D camera or Orthographic for a 2D-style size-based camera.
class Camera final
{
public:
    Camera() = default;

    void SetPosition(float x, float y, float z) noexcept;
    void SetPosition(const DirectX::XMFLOAT3& position) noexcept;
    [[nodiscard]] const DirectX::XMFLOAT3& Position() const noexcept;

    // Angles are radians.  Pitch is clamped short of +/- pi/2 so the
    // forward vector never becomes parallel to the world-up vector.
    void SetYawPitchRadians(float yawRadians, float pitchRadians) noexcept;
    void AddYawPitchRadians(
        float yawDeltaRadians,
        float pitchDeltaRadians) noexcept;
    [[nodiscard]] float YawRadians() const noexcept;
    [[nodiscard]] float PitchRadians() const noexcept;

    [[nodiscard]] DirectX::XMFLOAT3 Forward() const noexcept;
    [[nodiscard]] DirectX::XMFLOAT3 Right() const noexcept;
    [[nodiscard]] DirectX::XMFLOAT3 Up() const noexcept;

    // Perspective arguments are vertical FOV in radians, aspect ratio,
    // and positive near/far planes.  This also selects Perspective mode.
    void SetPerspective(
        float verticalFieldOfViewRadians,
        float aspectRatio,
        float nearPlane,
        float farPlane);
    void SetVerticalFieldOfViewRadians(float fieldOfViewRadians);
    void SetPerspectiveAspectRatio(float aspectRatio);

    // Orthographic width/height define the visible world size.  It is
    // suitable for 2D rendering; use SetOrthographicSize on resize when
    // the logical 2D viewport should follow the window dimensions.
    void SetOrthographic(
        float width,
        float height,
        float nearPlane,
        float farPlane);
    void SetOrthographicSize(float width, float height);
    void SetDepthRange(float nearPlane, float farPlane);

    [[nodiscard]] CameraProjectionType ProjectionType() const noexcept;
    [[nodiscard]] float VerticalFieldOfViewRadians() const noexcept;
    [[nodiscard]] float PerspectiveAspectRatio() const noexcept;
    [[nodiscard]] float OrthographicWidth() const noexcept;
    [[nodiscard]] float OrthographicHeight() const noexcept;
    [[nodiscard]] float NearPlane() const noexcept;
    [[nodiscard]] float FarPlane() const noexcept;

    [[nodiscard]] DirectX::XMMATRIX ViewMatrix() const noexcept;
    [[nodiscard]] DirectX::XMMATRIX ProjectionMatrix() const noexcept;
    [[nodiscard]] DirectX::XMMATRIX ViewProjectionMatrix() const noexcept;
    [[nodiscard]] const ViewFrustum& Frustum() const noexcept;

private:
    [[nodiscard]] static float ClampPitch(float pitchRadians) noexcept;
    void MarkViewDirty() noexcept;
    void MarkProjectionDirty() noexcept;
    void UpdateViewMatrix() const noexcept;
    void UpdateProjectionMatrix() const noexcept;
    void UpdateViewProjectionMatrix() const noexcept;
    void UpdateFrustum() const noexcept;

    DirectX::XMFLOAT3 position_{};
    float yawRadians_{};
    float pitchRadians_{};
    CameraProjectionType projectionType_{CameraProjectionType::Perspective};
    float verticalFieldOfViewRadians_{DirectX::XM_PI / 3.0F};
    float perspectiveAspectRatio_{1.0F};
    float orthographicWidth_{1.0F};
    float orthographicHeight_{1.0F};
    float nearPlane_{0.1F};
    float farPlane_{1000.0F};
    mutable DirectX::XMFLOAT4X4 viewMatrix_{};
    mutable DirectX::XMFLOAT4X4 projectionMatrix_{};
    mutable DirectX::XMFLOAT4X4 viewProjectionMatrix_{};
    mutable ViewFrustum frustum_{};
    mutable bool viewDirty_{true};
    mutable bool projectionDirty_{true};
    mutable bool viewProjectionDirty_{true};
    mutable bool frustumDirty_{true};
};

} // namespace PhantomCore
