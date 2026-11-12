#pragma once

#include "Visual2D/Visual2DNode.h"

#include <array>
#include <functional>
#include <optional>
#include <string_view>


namespace PhantomCore
{
class SpriteVisualComponent final : public Visual2DComponent
{
public:
    [[nodiscard]] const VisualStyle& Style() const noexcept;
    void SetStyle(const VisualStyle& style) noexcept;
    void SetImage(ImageHandle image) noexcept;
    void SetTint(Color tint) noexcept;
    [[nodiscard]] float CornerRadius() const noexcept;
    // Radius is measured in the node's local Canvas units. It applies to
    // both solid fills and images without changing layout or batching.
    void SetCornerRadius(float radius);
    [[nodiscard]] Float2 UvScale() const noexcept;
    [[nodiscard]] Float2 UvOffset() const noexcept;
    void SetUvTransform(
        Float2 scale,
        Float2 offset) noexcept;

    void AppendDrawPackets(
        std::vector<DrawPacket>& packets) const override;

private:
    [[nodiscard]] Color CurrentColor() const noexcept;
    [[nodiscard]] ImageHandle CurrentImage() const noexcept;

    VisualStyle style_{};
    Float2 uvScale_{1.0F, 1.0F};
    Float2 uvOffset_{};
    float cornerRadius_{};
};

class TextVisualComponent final : public Visual2DComponent
{
public:
    explicit TextVisualComponent(std::wstring text = {});

    [[nodiscard]] std::wstring_view Text() const noexcept;
    void SetText(std::wstring text);
    [[nodiscard]] const TextFont& Font() const noexcept;
    void SetFont(TextFont font);
    [[nodiscard]] float FontSize() const noexcept;
    void SetFontSize(float fontSize);
    [[nodiscard]] std::uint16_t FontWeight() const noexcept;
    // OpenType weight, 1..999; changing it invalidates shaping/measurement.
    void SetFontWeight(std::uint16_t weight);
    [[nodiscard]] Color TextColor() const noexcept;
    void SetTextColor(Color color) noexcept;
    [[nodiscard]] TextAlignment HorizontalAlignment() const noexcept;
    void SetHorizontalAlignment(TextAlignment alignment) noexcept;
    [[nodiscard]] Rect ContentBounds() const noexcept;
    void SetContentBounds(Rect bounds);
    [[nodiscard]] std::optional<Size> MeasuredSize() const noexcept;
    // Moves glyphs inside the unchanged layout/clip rectangle.
    void SetTextOffset(Point offset);
    // Optional second copy shares the same shaping result; its displacement
    // is relative to the first copy. Animation remains a Client responsibility.
    void SetRepeatOffset(std::optional<Point> offset);

    void AppendDrawPackets(
        std::vector<DrawPacket>& packets) const override;

private:
    std::wstring text_;
    TextFont font_;
    float fontSize_{18.0F};
    std::uint16_t fontWeight_{400};
    Color textColor_{1.0F, 1.0F, 1.0F, 1.0F};
    TextAlignment alignment_{TextAlignment::Leading};
    std::optional<Rect> contentBounds_;
    Point textOffset_{};
    std::optional<Point> repeatOffset_;
    mutable TextContentHandle content_;
};

class RectangleCollider2DComponent final : public Visual2DComponent
{
public:
    RectangleCollider2DComponent() = default;
    explicit RectangleCollider2DComponent(Rect localBounds);

    void SetLocalBounds(Rect localBounds);
    void UseNodeBounds() noexcept;
    [[nodiscard]] bool HitTest(Point localPosition) const noexcept override;

private:
    std::optional<Rect> localBounds_;
};

class CircleCollider2DComponent final : public Visual2DComponent
{
public:
    CircleCollider2DComponent(Point center, float radius);
    [[nodiscard]] Point Center() const noexcept;
    [[nodiscard]] float Radius() const noexcept;
    void SetCircle(Point center, float radius);
    [[nodiscard]] bool HitTest(Point localPosition) const noexcept override;

private:
    Point center_{};
    float radius_{};
};

using HitTestHandler = std::function<bool(
    const Visual2DNode&,
    Point)>;

// Strategy component for triangles, masks or game-specific collision.
class CustomCollider2DComponent final : public Visual2DComponent
{
public:
    explicit CustomCollider2DComponent(HitTestHandler handler);
    void SetHandler(HitTestHandler handler);
    [[nodiscard]] bool HitTest(Point localPosition) const override;

private:
    HitTestHandler handler_;
};

using PointerHandler = std::function<void(
    Visual2DNode&,
    const PointerEvent&,
    std::vector<Action>&)>;

class PointerReceiverComponent final : public Visual2DComponent
{
public:
    explicit PointerReceiverComponent(PointerHandler handler = {});
    void SetHandler(PointerHandler handler);
    void OnPointerEvent(
        const PointerEvent& event,
        std::vector<Action>& actions) override;

private:
    PointerHandler handler_;
};

using AnimationHandler = std::function<void(Visual2DNode&, double)>;

class AnimatorComponent final : public Visual2DComponent
{
public:
    explicit AnimatorComponent(AnimationHandler handler = {});
    void SetHandler(AnimationHandler handler);
    void Update(double elapsedSeconds) override;

private:
    AnimationHandler handler_;
};

class ButtonBehaviorComponent final : public Visual2DComponent
{
public:
    void OnPointerEvent(
        const PointerEvent& event,
        std::vector<Action>& actions) override;
};

class ToggleBehaviorComponent final : public Visual2DComponent
{
public:
    explicit ToggleBehaviorComponent(bool checked = false);
    [[nodiscard]] bool IsChecked() const noexcept;
    void SetChecked(bool checked) noexcept;

    void AppendDrawPackets(
        std::vector<DrawPacket>& packets) const override;
    void OnPointerEvent(
        const PointerEvent& event,
        std::vector<Action>& actions) override;

private:
    bool checked_{};
};

class SliderBehaviorComponent final : public Visual2DComponent
{
public:
    explicit SliderBehaviorComponent(float value = 0.0F);
    [[nodiscard]] float Value() const noexcept;
    void SetValue(float value) noexcept;

    void AppendDrawPackets(
        std::vector<DrawPacket>& packets) const override;
    void OnPointerEvent(
        const PointerEvent& event,
        std::vector<Action>& actions) override;

private:
    void UpdateFromPointer(
        const PointerEvent& event,
        std::vector<Action>& actions);
    float value_{};
};

class CycleSelectorBehaviorComponent final : public Visual2DComponent
{
public:
    void SetItems(std::vector<std::wstring> items);
    [[nodiscard]] const std::vector<std::wstring>& Items() const noexcept;
    [[nodiscard]] std::size_t SelectedIndex() const noexcept;
    void SetSelectedIndex(std::size_t index);
    void OnPointerEvent(
        const PointerEvent& event,
        std::vector<Action>& actions) override;

private:
    void RefreshText();
    std::vector<std::wstring> items_;
    std::size_t selectedIndex_{};
};

class ComboBoxBehaviorComponent final : public Visual2DComponent
{
public:
    void SetItems(std::vector<std::wstring> items);
    [[nodiscard]] const std::vector<std::wstring>& Items() const noexcept;
    [[nodiscard]] std::size_t SelectedIndex() const noexcept;
    void SetSelectedIndex(std::size_t index);
    void SetMaxVisibleItems(std::size_t maxVisibleItems);
    [[nodiscard]] std::size_t MaxVisibleItems() const noexcept;
    void SetItemHeight(float itemHeight);
    [[nodiscard]] float ItemHeight() const noexcept;
    [[nodiscard]] const TextFont& Font() const noexcept;
    void SetFont(TextFont font);
    void SetFontSize(float fontSize);
    [[nodiscard]] Color TextColor() const noexcept;
    void SetTextColor(Color color) noexcept;
    [[nodiscard]] Color SelectedTextColor() const noexcept;
    void SetSelectedTextColor(Color color) noexcept;
    [[nodiscard]] Color PopupBackgroundColor() const noexcept;
    void SetPopupBackgroundColor(Color color) noexcept;
    [[nodiscard]] bool IsExpanded() const noexcept;
    void Collapse() noexcept;

    void AppendDrawPackets(
        std::vector<DrawPacket>& packets) const override;
    [[nodiscard]] bool HitTest(Point localPosition) const noexcept override;
    void OnPointerEvent(
        const PointerEvent& event,
        std::vector<Action>& actions) override;

private:
    [[nodiscard]] Rect PopupBounds() const noexcept;
    void EnsureTextContent() const;
    [[nodiscard]] std::size_t VisibleItemCount() const noexcept;
    [[nodiscard]] std::optional<std::size_t> ItemIndexAt(
        Point localPosition) const noexcept;
    void EnsureSelectedItemVisible() noexcept;
    void ScrollBy(int itemDelta) noexcept;
    void UpdateHoveredItem(Point localPosition) noexcept;
    [[nodiscard]] const VisualStyle& OwnerStyle() const noexcept;

    std::vector<std::wstring> items_;
    mutable std::vector<TextContentHandle> itemContent_;
    mutable std::array<TextContentHandle, 2> arrowContent_;
    std::size_t selectedIndex_{};
    std::size_t firstVisibleIndex_{};
    std::size_t maxVisibleItems_{4};
    std::size_t hoveredItemIndex_{static_cast<std::size_t>(-1)};
    float itemHeight_{36.0F};
    TextFont font_;
    float fontSize_{17.0F};
    Color textColor_{1.0F, 1.0F, 1.0F, 1.0F};
    Color selectedTextColor_{1.0F, 1.0F, 1.0F, 1.0F};
    Color popupBackgroundColor_{0.055F, 0.070F, 0.105F, 0.98F};
    float dragStartY_{};
    std::size_t dragStartFirstVisibleIndex_{};
    bool expanded_{};
    bool trackingDrag_{};
    bool dragMoved_{};
};

} // namespace PhantomCore
