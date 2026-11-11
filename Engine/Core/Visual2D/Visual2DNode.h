#pragma once

// Backend-neutral 2D visual tree. A Visual2DNode is a concrete component host;
// sprites and widgets are component combinations rather than subclasses.

#include "System/TransformNode.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>


namespace PhantomCore
{
using NodeId = std::uint64_t;

struct ImageHandle
{
    std::uint64_t value{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value != 0;
    }
};

struct Point
{
    float x{};
    float y{};
};

struct Size
{
    float width{};
    float height{};
};

struct Rect
{
    float x{};
    float y{};
    float width{};
    float height{};

    [[nodiscard]] bool Contains(Point point) const noexcept;
};

struct Color
{
    float red{};
    float green{};
    float blue{};
    float alpha{1.0F};
};

struct VisualStyle
{
    Color normal{0.20F, 0.22F, 0.27F, 1.0F};
    Color hovered{0.28F, 0.32F, 0.40F, 1.0F};
    Color pressed{0.12F, 0.16F, 0.24F, 1.0F};
    Color disabled{0.14F, 0.14F, 0.16F, 0.65F};
    ImageHandle normalImage{};
    ImageHandle hoveredImage{};
    ImageHandle pressedImage{};
    ImageHandle disabledImage{};
};

enum class Anchor : std::uint8_t
{
    TopLeft,
    TopCenter,
    TopRight,
    MiddleLeft,
    Center,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

enum class DrawPacketType : std::uint8_t
{
    Rectangle,
    Image,
    Text,
    RectangleBatch,
};

enum class TextAlignment : std::uint8_t
{
    Leading,
    Center,
    Trailing,
};

enum class TextFontSource : std::uint8_t
{
    Default,
    System,
    File,
};

// Keeps font selection backend-neutral. Render backends resolve a system
// family or font file into their native font handle and cache the result.
struct TextFont
{
    TextFontSource source{TextFontSource::Default};
    std::wstring value;
};

// Immutable content/style shared by draw packets. Render-owned measurement
// is published on the same engine thread as Visual2D Update/Submit.
struct TextContent
{
    std::wstring value;
    TextFont font;
    float fontSize{18.0F};
    TextAlignment alignment{TextAlignment::Leading};
    mutable std::optional<Size> measuredSize;
    std::uint16_t fontWeight{400};
};
using TextContentHandle = std::shared_ptr<const TextContent>;

struct RectanglePrimitive
{
    Rect bounds{};
    Color color{};
    float cornerRadius{};
};

// Bounds are local to the node. nodeTransform maps those local coordinates
// into the Canvas; render backends never traverse or own the node tree.
struct DrawPacket
{
    DrawPacketType type{DrawPacketType::Rectangle};
    Rect bounds{};
    DirectX::XMFLOAT4X4 nodeTransform{};
    Color color{};
    TextContentHandle text;
    Point textOffset{};
    ImageHandle image{};
    DirectX::XMFLOAT2 uvScale{1.0F, 1.0F};
    DirectX::XMFLOAT2 uvOffset{};
    float cornerRadius{};
    // Optional axis-aligned clip rectangle in Canvas coordinates. Core
    // resolves nested node-local clips before packets reach a backend.
    std::optional<Rect> clipBounds;
    // Immutable local geometry is shared across frames; tree collection copies
    // only this handle. Primitive order, transform and clipping are preserved.
    std::shared_ptr<const std::vector<RectanglePrimitive>> rectangles;
};

inline std::size_t DrawPrimitiveCount(std::span<const DrawPacket> packets) noexcept
{
    std::size_t count = 0;
    for (const auto &packet : packets)
        count += packet.type == DrawPacketType::RectangleBatch
            ? (packet.rectangles ? packet.rectangles->size() : 0) : 1;
    return count;
}

template<class Visitor>
void ForEachDrawPrimitive(std::span<const DrawPacket> packets, Visitor &&visitor)
{
    for (const auto &packet : packets)
    {
        if (packet.type != DrawPacketType::RectangleBatch)
        {
            visitor(packet);
            continue;
        }
        if (!packet.rectangles)
            continue;
        DrawPacket primitive;
        primitive.nodeTransform = packet.nodeTransform;
        primitive.clipBounds = packet.clipBounds;
        for (const auto &rectangle : *packet.rectangles)
        {
            primitive.bounds = rectangle.bounds;
            primitive.color = rectangle.color;
            primitive.cornerRadius = rectangle.cornerRadius;
            visitor(primitive);
        }
    }
}

enum class PointerEventType : std::uint8_t
{
    Enter,
    Leave,
    Move,
    Wheel,
    Press,
    Release,
    Click,
};

enum class PointerButton : std::uint8_t
{
    None,
    Left,
    Right,
    Middle,
};

struct PointerEvent
{
    PointerEventType type{};
    PointerButton button{PointerButton::None};
    Point canvasPosition{};
    Point localPosition{};
    std::int64_t timestampTicks{};
    float wheelDelta{};
};

enum class ActionType : std::uint8_t
{
    Clicked,
    ValueChanged,
    SelectionChanged,
};

struct Action
{
    ActionType type{};
    NodeId source{};
    float value{};
    std::size_t selectedIndex{};
    std::int64_t timestampTicks{};
};

class Visual2DNode;
class Visual2DCanvas;
class Visual2DInputRouter;

class Visual2DComponent
{
public:
    virtual ~Visual2DComponent();

    Visual2DComponent(const Visual2DComponent&) = delete;
    Visual2DComponent& operator=(const Visual2DComponent&) = delete;

    [[nodiscard]] Visual2DNode& Owner() noexcept;
    [[nodiscard]] const Visual2DNode& Owner() const noexcept;

    virtual void Update(double elapsedSeconds);
    virtual void AppendDrawPackets(std::vector<DrawPacket>& packets) const;
    [[nodiscard]] virtual bool HitTest(Point localPosition) const noexcept;
    virtual void OnPointerEvent(
        const PointerEvent& event,
        std::vector<Action>& actions);

protected:
    Visual2DComponent() = default;

private:
    friend class Visual2DNode;
    Visual2DNode* owner_{};
};

class Visual2DNode final
{
public:
    explicit Visual2DNode(std::string name = {});
    ~Visual2DNode();

    Visual2DNode(const Visual2DNode&) = delete;
    Visual2DNode& operator=(const Visual2DNode&) = delete;

    [[nodiscard]] NodeId Id() const noexcept;
    [[nodiscard]] const std::string& Name() const noexcept;
    void SetName(std::string name);

    [[nodiscard]] TransformNode& Transform() noexcept;
    [[nodiscard]] const TransformNode& Transform() const noexcept;
    // Position is measured from the selected parent/anchor to this node's
    // normalized Pivot. It is converted to the Transform's lower-left
    // position whenever position, size or Pivot changes.
    [[nodiscard]] Point Position() const noexcept;
    void SetPosition(Point position);
    [[nodiscard]] Size NodeSize() const noexcept;
    void SetSize(Size size);
    [[nodiscard]] Point Pivot() const noexcept;
    void SetPivot(Point normalizedPivot);
    [[nodiscard]] Rect Bounds() const noexcept;
    void SetBounds(Rect bounds);
    [[nodiscard]] Rect BoundsInCanvas() const;

    [[nodiscard]] std::int32_t ZIndex() const noexcept;
    void SetZIndex(std::int32_t zIndex) noexcept;
    [[nodiscard]] bool IsVisible() const noexcept;
    void SetVisible(bool visible) noexcept;
    [[nodiscard]] bool IsEnabled() const noexcept;
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] const std::optional<Rect>& ClipRect() const noexcept;
    // Clips this node's complete subtree to a node-local rectangle.
    // Set {0, 0, width, height} to clip children to the node bounds.
    void SetClipRect(Rect localRect);
    void ClearClipRect() noexcept;
    [[nodiscard]] bool IsHovered() const noexcept;
    [[nodiscard]] bool IsPressed() const noexcept;

    [[nodiscard]] Visual2DNode* Parent() noexcept;
    [[nodiscard]] const Visual2DNode* Parent() const noexcept;
    [[nodiscard]] const std::vector<std::unique_ptr<Visual2DNode>>&
        Children() const noexcept;
    Visual2DNode& AddChild(std::unique_ptr<Visual2DNode> child);
    [[nodiscard]] Visual2DNode& CreateChild(std::string name = {});
    [[nodiscard]] bool RemoveChild(NodeId id) noexcept;

    template <typename ComponentType, typename... ArgumentTypes>
        requires std::is_base_of_v<Visual2DComponent, ComponentType>
    ComponentType& AddComponent(ArgumentTypes&&... arguments)
    {
        if (GetComponent<ComponentType>() != nullptr)
        {
            throw std::logic_error(
                "A Visual2D node cannot contain the same component type twice.");
        }
        auto component = std::make_unique<ComponentType>(
            std::forward<ArgumentTypes>(arguments)...);
        component->owner_ = this;
        ComponentType& result = *component;
        components_.push_back(std::move(component));
        return result;
    }

    template <typename ComponentType>
        requires std::is_base_of_v<Visual2DComponent, ComponentType>
    [[nodiscard]] ComponentType* GetComponent() noexcept
    {
        for (const std::unique_ptr<Visual2DComponent>& component :
            components_)
        {
            if (auto* result = dynamic_cast<ComponentType*>(component.get()))
            {
                return result;
            }
        }
        return nullptr;
    }

    template <typename ComponentType>
        requires std::is_base_of_v<Visual2DComponent, ComponentType>
    [[nodiscard]] const ComponentType* GetComponent() const noexcept
    {
        return const_cast<Visual2DNode*>(this)->GetComponent<ComponentType>();
    }

    template <typename ComponentType>
        requires std::is_base_of_v<Visual2DComponent, ComponentType>
    [[nodiscard]] bool RemoveComponent() noexcept
    {
        for (auto iterator = components_.begin();
            iterator != components_.end(); ++iterator)
        {
            if (dynamic_cast<ComponentType*>(iterator->get()) != nullptr)
            {
                components_.erase(iterator);
                return true;
            }
        }
        return false;
    }

private:
    friend class Visual2DCanvas;
    friend class Visual2DInputRouter;

    struct HitResult
    {
        Visual2DNode* node{};
        Point localPosition{};
    };

    [[nodiscard]] HitResult HitTest(
        Point canvasPosition,
        const std::optional<Rect>& inheritedClip = std::nullopt) noexcept;
    [[nodiscard]] bool MapCanvasPointToLocal(
        Point canvasPosition,
        Point& localPosition) noexcept;
    [[nodiscard]] Visual2DNode* Find(NodeId id) noexcept;
    [[nodiscard]] const Visual2DNode* Find(NodeId id) const noexcept;
    void UpdateRecursive(double elapsedSeconds);
    void CollectDrawPackets(
        std::vector<DrawPacket>& packets,
        const std::optional<Rect>& inheritedClip = std::nullopt) const;
    void DispatchPointerEvent(
        const PointerEvent& event,
        std::vector<Action>& actions);
    void InvalidatePaintOrder() noexcept;
    void EnsurePaintOrder() const;
    void SetHovered(bool hovered) noexcept;
    void SetPressed(bool pressed) noexcept;
    void UpdateTransformLayout() noexcept;

    NodeId id_{};
    std::string name_;
    TransformNode transform_;
    Point position_{};
    Size size_{};
    Point pivot_{};
    std::int32_t zIndex_{};
    Visual2DNode* parent_{};
    std::vector<std::unique_ptr<Visual2DNode>> children_;
    mutable std::vector<Visual2DNode*> paintOrder_;
    mutable bool paintOrderDirty_{true};
    std::vector<std::unique_ptr<Visual2DComponent>> components_;
    std::optional<Rect> clipRect_;
    bool visible_{true};
    bool enabled_{true};
    bool hovered_{};
    bool pressed_{};
};

} // namespace PhantomCore
