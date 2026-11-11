#include "Visual2D/Visual2DNode.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <ranges>

namespace PhantomCore
{
using namespace DirectX;


namespace
{
    std::atomic<NodeId> nextNodeId{1};

    [[nodiscard]] bool IsFinite(const Point point) noexcept
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    }

    [[nodiscard]] bool IsValidSize(const Size size) noexcept
    {
        return std::isfinite(size.width) && std::isfinite(size.height) &&
            size.width >= 0.0F && size.height >= 0.0F;
    }

    [[nodiscard]] Rect TransformRect(
        const Rect rect,
        const XMMATRIX& transform) noexcept
    {
        const XMVECTOR corners[] = {
            XMVectorSet(rect.x, rect.y, 0.0F, 1.0F),
            XMVectorSet(rect.x + rect.width, rect.y, 0.0F, 1.0F),
            XMVectorSet(rect.x, rect.y + rect.height, 0.0F, 1.0F),
            XMVectorSet(
                rect.x + rect.width,
                rect.y + rect.height,
                0.0F,
                1.0F)};
        float minimumX = std::numeric_limits<float>::max();
        float minimumY = std::numeric_limits<float>::max();
        float maximumX = std::numeric_limits<float>::lowest();
        float maximumY = std::numeric_limits<float>::lowest();
        for (const XMVECTOR corner : corners)
        {
            const XMVECTOR transformed =
                XMVector3TransformCoord(corner, transform);
            minimumX = std::min(minimumX, XMVectorGetX(transformed));
            minimumY = std::min(minimumY, XMVectorGetY(transformed));
            maximumX = std::max(maximumX, XMVectorGetX(transformed));
            maximumY = std::max(maximumY, XMVectorGetY(transformed));
        }
        return {
            minimumX,
            minimumY,
            maximumX - minimumX,
            maximumY - minimumY};
    }

    [[nodiscard]] std::optional<Rect> IntersectRects(
        const std::optional<Rect>& first,
        const std::optional<Rect>& second) noexcept
    {
        if (!first.has_value())
        {
            return second;
        }
        if (!second.has_value())
        {
            return first;
        }
        const float left = std::max(first->x, second->x);
        const float bottom = std::max(first->y, second->y);
        const float right = std::min(
            first->x + first->width,
            second->x + second->width);
        const float top = std::min(
            first->y + first->height,
            second->y + second->height);
        return Rect{
            left,
            bottom,
            std::max(right - left, 0.0F),
            std::max(top - bottom, 0.0F)};
    }
}

bool Rect::Contains(const Point point) const noexcept
{
    return point.x >= x && point.y >= y &&
        point.x <= x + width && point.y <= y + height;
}

struct Visual2DNode::UpdateContext final
{
    explicit UpdateContext(Visual2DNode& owner) : root(owner)
    {
        if (root.activeUpdate_ != nullptr)
            throw std::logic_error("A Visual2D tree cannot be updated recursively.");
        root.activeUpdate_ = this;
    }
    ~UpdateContext()
    {
        running = false;
        root.activeUpdate_ = nullptr;
        Visual2DNode::DestroyRetired(*this);
    }

    Visual2DNode& root;
    bool running{true};
    std::size_t removalRevision{};
    std::unique_ptr<Visual2DComponent> components;
    Visual2DComponent* componentTail{};
    std::unique_ptr<Visual2DNode> nodes;
    Visual2DNode* nodeTail{};
};


Visual2DComponent::~Visual2DComponent() = default;

Visual2DNode& Visual2DComponent::Owner() noexcept
{
    return *owner_;
}

const Visual2DNode& Visual2DComponent::Owner() const noexcept
{
    return *owner_;
}

void Visual2DComponent::Update(const double)
{
}

void Visual2DComponent::AppendDrawPackets(
    std::vector<DrawPacket>&) const
{
}

bool Visual2DComponent::HitTest(const Point) const
{
    return false;
}

void Visual2DComponent::OnPointerEvent(
    const PointerEvent&,
    std::vector<Action>&)
{
}

Visual2DNode::Visual2DNode(std::string name)
    : id_(nextNodeId.fetch_add(1, std::memory_order_relaxed)),
      name_(std::move(name))
{
}

Visual2DNode::~Visual2DNode() = default;

NodeId Visual2DNode::Id() const noexcept
{
    return id_;
}

const std::string& Visual2DNode::Name() const noexcept
{
    return name_;
}

void Visual2DNode::SetName(std::string name)
{
    name_ = std::move(name);
}

TransformNode& Visual2DNode::Transform() noexcept
{
    return transform_;
}

const TransformNode& Visual2DNode::Transform() const noexcept
{
    return transform_;
}

Point Visual2DNode::Position() const noexcept
{
    return position_;
}

void Visual2DNode::SetPosition(const Point position)
{
    if (!IsFinite(position))
    {
        throw std::invalid_argument("Visual2D position must be finite.");
    }
    if (position_.x == position.x && position_.y == position.y)
    {
        return;
    }
    position_ = position;
    UpdateTransformLayout();
}

Size Visual2DNode::NodeSize() const noexcept
{
    return size_;
}

void Visual2DNode::SetSize(const Size size)
{
    if (!IsValidSize(size))
    {
        throw std::invalid_argument(
            "Visual2D node size must be finite and non-negative.");
    }
    if (size_.width == size.width && size_.height == size.height)
    {
        return;
    }
    size_ = size;
    UpdateTransformLayout();
}

Point Visual2DNode::Pivot() const noexcept
{
    return pivot_;
}

void Visual2DNode::SetPivot(const Point normalizedPivot)
{
    if (!IsFinite(normalizedPivot))
    {
        throw std::invalid_argument("Visual2D pivot must be finite.");
    }
    if (pivot_.x == normalizedPivot.x &&
        pivot_.y == normalizedPivot.y)
    {
        return;
    }
    pivot_ = normalizedPivot;
    UpdateTransformLayout();
}

Rect Visual2DNode::Bounds() const noexcept
{
    return {position_.x, position_.y, size_.width, size_.height};
}

void Visual2DNode::SetBounds(const Rect bounds)
{
    if (!IsFinite({bounds.x, bounds.y}) ||
        !IsValidSize({bounds.width, bounds.height}))
    {
        throw std::invalid_argument(
            "Visual2D bounds must be finite and non-negative.");
    }
    if (position_.x == bounds.x && position_.y == bounds.y &&
        size_.width == bounds.width && size_.height == bounds.height)
    {
        return;
    }
    position_ = {bounds.x, bounds.y};
    size_ = {bounds.width, bounds.height};
    UpdateTransformLayout();
}

Rect Visual2DNode::BoundsInCanvas() const
{
    return TransformRect(
        {0.0F, 0.0F, size_.width, size_.height},
        XMLoadFloat4x4(&transform_.WorldMatrix()));
}

std::int32_t Visual2DNode::ZIndex() const noexcept
{
    return zIndex_;
}

void Visual2DNode::SetZIndex(const std::int32_t zIndex) noexcept
{
    if (zIndex_ == zIndex)
    {
        return;
    }
    zIndex_ = zIndex;
    if (parent_ != nullptr)
    {
        parent_->InvalidatePaintOrder();
    }
}

bool Visual2DNode::IsVisible() const noexcept
{
    return visible_;
}

void Visual2DNode::SetVisible(const bool visible) noexcept
{
    visible_ = visible;
    if (!visible_)
    {
        hovered_ = false;
        pressed_ = false;
    }
}

bool Visual2DNode::IsEnabled() const noexcept
{
    return enabled_;
}

void Visual2DNode::SetEnabled(const bool enabled) noexcept
{
    enabled_ = enabled;
    if (!enabled_)
    {
        hovered_ = false;
        pressed_ = false;
    }
}

const std::optional<Rect>& Visual2DNode::ClipRect() const noexcept
{
    return clipRect_;
}

void Visual2DNode::SetClipRect(const Rect localRect)
{
    if (!IsFinite({localRect.x, localRect.y}) ||
        !IsValidSize({localRect.width, localRect.height}))
    {
        throw std::invalid_argument(
            "Visual2D clip bounds must be finite and non-negative.");
    }
    clipRect_ = localRect;
}

void Visual2DNode::ClearClipRect() noexcept
{
    clipRect_.reset();
}

bool Visual2DNode::IsHovered() const noexcept
{
    return hovered_;
}

bool Visual2DNode::IsPressed() const noexcept
{
    return pressed_;
}

Visual2DNode* Visual2DNode::Parent() noexcept
{
    return parent_;
}

const Visual2DNode* Visual2DNode::Parent() const noexcept
{
    return parent_;
}

const std::vector<std::unique_ptr<Visual2DNode>>&
Visual2DNode::Children() const noexcept
{
    return children_;
}

Visual2DNode& Visual2DNode::AddChild(
    std::unique_ptr<Visual2DNode> child)
{
    if (child == nullptr || child->parent_ != nullptr)
    {
        throw std::invalid_argument(
            "A Visual2D child must be non-null and unattached.");
    }
    child->parent_ = this;
    child->transform_.SetParent(&transform_);
    Visual2DNode& result = *child;
    children_.push_back(std::move(child));
    InvalidatePaintOrder();
    return result;
}

Visual2DNode& Visual2DNode::CreateChild(std::string name)
{
    return AddChild(std::make_unique<Visual2DNode>(std::move(name)));
}

bool Visual2DNode::RemoveChild(const NodeId id) noexcept
{
    const auto iterator = std::ranges::find_if(
        children_, [id](const auto& child) { return child->Id() == id; });
    if (iterator == children_.end())
        return false;

    auto* context = ActiveUpdate();
    auto removed = std::move(*iterator);
    removed->transform_.SetParent(nullptr);
    removed->parent_ = nullptr;
    children_.erase(iterator);
    InvalidatePaintOrder();
    if (context != nullptr)
    {
        removed->retired_ = true;
        removed->activeUpdate_ = context;
        auto* tail = removed.get();
        if (context->nodeTail != nullptr)
            context->nodeTail->retiredNext_ = std::move(removed);
        else
            context->nodes = std::move(removed);
        context->nodeTail = tail;
        ++context->removalRevision;
    }
    return true;
}

Visual2DNode::HitResult Visual2DNode::HitTest(
    const Point canvasPosition,
    const std::optional<Rect>& inheritedClip)
{
    if (!visible_ || !enabled_)
    {
        return {};
    }

    std::optional<Rect> activeClip = inheritedClip;
    if (clipRect_.has_value())
    {
        activeClip = IntersectRects(
            activeClip,
            TransformRect(
                *clipRect_,
                XMLoadFloat4x4(&transform_.WorldMatrix())));
    }
    if (activeClip.has_value() && !activeClip->Contains(canvasPosition))
    {
        return {};
    }

    EnsurePaintOrder();
    for (auto iterator = paintOrder_.rbegin();
        iterator != paintOrder_.rend(); ++iterator)
    {
        HitResult childHit = (*iterator)->HitTest(
            canvasPosition,
            activeClip);
        if (childHit.node != nullptr)
        {
            return childHit;
        }
    }

    Point local{};
    if (!MapCanvasPointToLocal(canvasPosition, local))
    {
        return {};
    }
    for (const std::unique_ptr<Visual2DComponent>& component : components_)
    {
        if (component->HitTest(local))
        {
            return {this, local};
        }
    }
    return {};
}

bool Visual2DNode::MapCanvasPointToLocal(
    const Point canvasPosition,
    Point& localPosition) noexcept
{
    XMVECTOR determinant{};
    const XMMATRIX inverse = XMMatrixInverse(
        &determinant,
        XMLoadFloat4x4(&transform_.WorldMatrix()));
    if (std::abs(XMVectorGetX(determinant)) <=
        std::numeric_limits<float>::epsilon())
    {
        return false;
    }

    const XMVECTOR origin = XMVector3TransformCoord(
        XMVectorSet(canvasPosition.x, canvasPosition.y, -10000.0F, 1.0F),
        inverse);
    const XMVECTOR direction = XMVector3TransformNormal(
        XMVectorSet(0.0F, 0.0F, 1.0F, 0.0F),
        inverse);
    const float directionZ = XMVectorGetZ(direction);
    if (std::abs(directionZ) <= std::numeric_limits<float>::epsilon())
    {
        return false;
    }
    const float parameter = -XMVectorGetZ(origin) / directionZ;
    if (parameter < 0.0F)
    {
        return false;
    }
    const XMVECTOR hit = XMVectorMultiplyAdd(
        XMVectorReplicate(parameter), direction, origin);
    localPosition = {XMVectorGetX(hit), XMVectorGetY(hit)};
    return std::isfinite(localPosition.x) && std::isfinite(localPosition.y);
}

Visual2DNode* Visual2DNode::Find(const NodeId id) noexcept
{
    if (id_ == id)
    {
        return this;
    }
    for (const std::unique_ptr<Visual2DNode>& child : children_)
    {
        if (Visual2DNode* found = child->Find(id); found != nullptr)
        {
            return found;
        }
    }
    return nullptr;
}

const Visual2DNode* Visual2DNode::Find(const NodeId id) const noexcept
{
    return const_cast<Visual2DNode*>(this)->Find(id);
}

Visual2DNode::UpdateContext* Visual2DNode::ActiveUpdate() noexcept
{
    for (auto* node = this; node != nullptr; node = node->parent_)
    {
        if (node->activeUpdate_ != nullptr)
            return node->activeUpdate_->running ? node->activeUpdate_ : nullptr;
    }
    return nullptr;
}

bool Visual2DNode::HasRetiredAncestor() const noexcept
{
    for (auto* node = this; node != nullptr; node = node->parent_)
    {
        if (node->retired_)
            return true;
    }
    return false;
}

void Visual2DNode::RetireComponent(std::unique_ptr<Visual2DComponent> component) noexcept
{
    if (auto* context = ActiveUpdate())
    {
        auto* tail = component.get();
        if (context->componentTail != nullptr)
            context->componentTail->retiredNext_ = std::move(component);
        else
            context->components = std::move(component);
        context->componentTail = tail;
        ++context->removalRevision;
    }
    // Outside Update, the local owner destroys the removed component now.
}

void Visual2DNode::DestroyRetired(UpdateContext& context) noexcept
{
    // Removed components may inspect their still-live Owner in destruction.
    // Unlink each entry before destruction to avoid a recursive queue teardown.
    while (context.components)
    {
        auto component = std::move(context.components);
        context.components = std::move(component->retiredNext_);
    }
    while (context.nodes)
    {
        auto node = std::move(context.nodes);
        context.nodes = std::move(node->retiredNext_);
    }
}

void Visual2DNode::UpdateRecursive(const double elapsedSeconds)
{
    UpdateContext context(*this);
    UpdateRecursive(elapsedSeconds, context);
}

void Visual2DNode::UpdateRecursive(
    const double elapsedSeconds, UpdateContext& context)
{
    if (!visible_)
        return;

    // Mark only entries present when this phase starts. Additions before a
    // later node/child phase are eligible there; additions during it wait.
    for (const auto& component : components_)
        component->updatePending_ = true;
    std::size_t index{};
    while (index < components_.size())
    {
        auto* component = components_[index].get();
        if (!component->updatePending_)
        {
            ++index;
            continue;
        }
        component->updatePending_ = false;
        const auto revision = context.removalRevision;
        component->Update(elapsedSeconds);
        if (context.removalRevision != revision && HasRetiredAncestor())
            return;
        // Vector storage/indices may have changed. The object remains alive
        // until the root Update ends, so comparing its address is safe.
        index = index < components_.size() && components_[index].get() == component
            ? index + 1 : 0;
    }

    for (const auto& child : children_)
        child->updatePending_ = true;
    index = 0;
    while (index < children_.size())
    {
        auto* child = children_[index].get();
        if (!child->updatePending_)
        {
            ++index;
            continue;
        }
        child->updatePending_ = false;
        const auto revision = context.removalRevision;
        child->UpdateRecursive(elapsedSeconds, context);
        if (context.removalRevision != revision && HasRetiredAncestor())
            return;
        index = index < children_.size() && children_[index].get() == child
            ? index + 1 : 0;
    }
}

void Visual2DNode::CollectDrawPackets(
    std::vector<DrawPacket>& packets,
    const std::optional<Rect>& inheritedClip) const
{
    if (!visible_)
    {
        return;
    }
    std::optional<Rect> activeClip = inheritedClip;
    if (clipRect_.has_value())
    {
        activeClip = IntersectRects(
            activeClip,
            TransformRect(
                *clipRect_,
                XMLoadFloat4x4(&transform_.WorldMatrix())));
    }
    if (activeClip.has_value() &&
        (activeClip->width <= 0.0F || activeClip->height <= 0.0F))
    {
        return;
    }
    const std::size_t firstPacket = packets.size();
    for (const std::unique_ptr<Visual2DComponent>& component : components_)
    {
        component->AppendDrawPackets(packets);
    }
    const XMFLOAT4X4 world = transform_.WorldMatrix();
    for (std::size_t index = firstPacket; index < packets.size(); ++index)
    {
        packets[index].nodeTransform = world;
        packets[index].clipBounds = activeClip;
    }

    EnsurePaintOrder();
    for (const Visual2DNode* child : paintOrder_)
    {
        child->CollectDrawPackets(packets, activeClip);
    }
}

void Visual2DNode::DispatchPointerEvent(
    const PointerEvent& event,
    std::vector<Action>& actions)
{
    for (const std::unique_ptr<Visual2DComponent>& component : components_)
    {
        component->OnPointerEvent(event, actions);
    }
}

void Visual2DNode::InvalidatePaintOrder() noexcept
{
    paintOrderDirty_ = true;
}

void Visual2DNode::EnsurePaintOrder() const
{
    if (!paintOrderDirty_)
    {
        return;
    }

    paintOrder_.clear();
    paintOrder_.reserve(children_.size());
    for (const std::unique_ptr<Visual2DNode>& child : children_)
    {
        paintOrder_.push_back(child.get());
    }
    std::stable_sort(
        paintOrder_.begin(),
        paintOrder_.end(),
        [](const Visual2DNode* left, const Visual2DNode* right)
        {
            return left->zIndex_ < right->zIndex_;
        });
    paintOrderDirty_ = false;
}

void Visual2DNode::SetHovered(const bool hovered) noexcept
{
    hovered_ = hovered;
}

void Visual2DNode::SetPressed(const bool pressed) noexcept
{
    pressed_ = pressed;
}

void Visual2DNode::UpdateTransformLayout() noexcept
{
    transform_.SetPivot(
        size_.width * pivot_.x,
        size_.height * pivot_.y,
        0.0F);
    transform_.SetPosition(
        position_.x - size_.width * pivot_.x,
        position_.y - size_.height * pivot_.y,
        transform_.Position().z);
}

} // namespace PhantomCore
