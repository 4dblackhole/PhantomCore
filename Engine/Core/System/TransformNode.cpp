#include "System/TransformNode.h"

#include <algorithm>
#include <stdexcept>

namespace PhantomCore
{
using namespace DirectX;


namespace
{
    [[nodiscard]] bool Equal(
        const XMFLOAT3& value,
        const float x,
        const float y,
        const float z) noexcept
    {
        return value.x == x && value.y == y && value.z == z;
    }

    [[nodiscard]] bool Equal(
        const XMFLOAT4& left,
        const XMFLOAT4& right) noexcept
    {
        return left.x == right.x && left.y == right.y &&
            left.z == right.z && left.w == right.w;
    }
}

TransformNode::TransformNode()
{
    XMStoreFloat4x4(&world_, XMMatrixIdentity());
}

TransformNode::~TransformNode()
{
    SetParent(nullptr);
    for (TransformNode* child : children_)
    {
        child->parent_ = nullptr;
        child->MarkWorldDirty();
    }
}

void TransformNode::SetPosition(
    const float x,
    const float y,
    const float z) noexcept
{
    if (Equal(position_, x, y, z))
    {
        return;
    }
    position_ = {x, y, z};
    MarkWorldDirty();
}

void TransformNode::SetScale(
    const float x,
    const float y,
    const float z) noexcept
{
    if (Equal(scale_, x, y, z))
    {
        return;
    }
    scale_ = {x, y, z};
    MarkWorldDirty();
}

void TransformNode::SetPivot(
    const float x,
    const float y,
    const float z) noexcept
{
    if (Equal(pivot_, x, y, z))
    {
        return;
    }
    pivot_ = {x, y, z};
    MarkWorldDirty();
}

void TransformNode::SetRotationRollPitchYaw(
    const float pitch,
    const float yaw,
    const float roll) noexcept
{
    XMFLOAT4 rotation{};
    XMStoreFloat4(
        &rotation,
        XMQuaternionRotationRollPitchYaw(pitch, yaw, roll));
    if (Equal(rotation_, rotation))
    {
        return;
    }
    rotation_ = rotation;
    MarkWorldDirty();
}

void TransformNode::SetRotationQuaternion(
    const float x,
    const float y,
    const float z,
    const float w) noexcept
{
    XMFLOAT4 rotation{};
    XMStoreFloat4(
        &rotation,
        XMQuaternionNormalize(XMVectorSet(x, y, z, w)));
    if (Equal(rotation_, rotation))
    {
        return;
    }
    rotation_ = rotation;
    MarkWorldDirty();
}

const XMFLOAT3& TransformNode::Position() const noexcept
{
    return position_;
}

const XMFLOAT3& TransformNode::Scale() const noexcept
{
    return scale_;
}

const XMFLOAT3& TransformNode::Pivot() const noexcept
{
    return pivot_;
}

const XMFLOAT4& TransformNode::Rotation() const noexcept
{
    return rotation_;
}

void TransformNode::SetParent(TransformNode* parent)
{
    if (parent == parent_)
    {
        return;
    }

    for (TransformNode* ancestor = parent;
        ancestor != nullptr;
        ancestor = ancestor->parent_)
    {
        if (ancestor == this)
        {
            throw std::invalid_argument(
                "A transform hierarchy cannot contain a cycle.");
        }
    }

    if (parent_ != nullptr)
    {
        std::erase(parent_->children_, this);
    }
    parent_ = parent;
    if (parent_ != nullptr)
    {
        parent_->children_.push_back(this);
    }
    MarkWorldDirty();
}

TransformNode* TransformNode::Parent() const noexcept
{
    return parent_;
}

const std::vector<TransformNode*>&
TransformNode::Children() const noexcept
{
    return children_;
}

const XMFLOAT4X4& TransformNode::WorldMatrix() const
{
    UpdateWorld();
    return world_;
}

void TransformNode::UpdateWorldRecursive()
{
    UpdateWorld();
    for (TransformNode* child : children_)
    {
        child->UpdateWorldRecursive();
    }
}

void TransformNode::MarkWorldDirty() noexcept
{
    // A parent transform affects every descendant world matrix, so defer
    // recomputation until WorldMatrix()/UpdateWorldRecursive is requested.
    if (worldDirty_)
    {
        return;
    }
    worldDirty_ = true;
    for (TransformNode* child : children_)
    {
        child->MarkWorldDirty();
    }
}

void TransformNode::UpdateWorld() const
{
    if (!worldDirty_)
    {
        return;
    }

    const XMMATRIX local =
        XMMatrixTranslation(-pivot_.x, -pivot_.y, -pivot_.z) *
        XMMatrixScaling(scale_.x, scale_.y, scale_.z) *
        XMMatrixRotationQuaternion(XMLoadFloat4(&rotation_)) *
        XMMatrixTranslation(
            position_.x + pivot_.x,
            position_.y + pivot_.y,
            position_.z + pivot_.z);

    if (parent_ != nullptr)
    {
        parent_->UpdateWorld();
        XMStoreFloat4x4(
            &world_,
            local * XMLoadFloat4x4(&parent_->world_));
    }
    else
    {
        XMStoreFloat4x4(&world_, local);
    }

    worldDirty_ = false;
}

} // namespace PhantomCore
