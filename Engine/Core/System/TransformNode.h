#pragma once

// System feature: reusable hierarchical local and world transforms.

#include "../../Math/Math.h"

#include <vector>


// Public hierarchical local/world transform used by Client scene objects.
namespace PhantomCore
{
class TransformNode final
{
public:
    TransformNode();
    ~TransformNode();

    TransformNode(const TransformNode&) = delete;
    TransformNode& operator=(const TransformNode&) = delete;

    void SetPosition(float x, float y, float z) noexcept;
    void SetScale(float x, float y, float z) noexcept;
    // Pivot is expressed in the node's local units. Position continues to
    // identify the unrotated local origin, so existing 3D users keep the
    // same behavior while 2D nodes can rotate around any point.
    void SetPivot(float x, float y, float z) noexcept;
    void SetRotationRollPitchYaw(
        float pitch,
        float yaw,
        float roll) noexcept;
    void SetRotationQuaternion(
        float x,
        float y,
        float z,
        float w) noexcept;

    [[nodiscard]] const Float3& Position() const noexcept;
    [[nodiscard]] const Float3& Scale() const noexcept;
    [[nodiscard]] const Float3& Pivot() const noexcept;
    [[nodiscard]] const Float4& Rotation() const noexcept;

    // Transform links never own either endpoint. The object/scene tree is
    // the single lifetime owner and must outlive its TransformNode links.
    void SetParent(TransformNode* parent);
    [[nodiscard]] TransformNode* Parent() const noexcept;
    [[nodiscard]] const std::vector<TransformNode*>&
        Children() const noexcept;

    [[nodiscard]] const Float4x4& WorldMatrix() const;
    void UpdateWorldRecursive();

private:
    void MarkWorldDirty() noexcept;
    void UpdateWorld() const;

    Float3 position_{0.0F, 0.0F, 0.0F};
    Float3 scale_{1.0F, 1.0F, 1.0F};
    Float3 pivot_{};
    Float4 rotation_{0.0F, 0.0F, 0.0F, 1.0F};
    mutable Float4x4 world_{};
    TransformNode* parent_{};
    std::vector<TransformNode*> children_;
    mutable bool worldDirty_{true};
};

} // namespace PhantomCore
