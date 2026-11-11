#include "Visual2D/Visual2DComponents.h"

#include <algorithm>
#include <cmath>


namespace PhantomCore
{
namespace
{
    constexpr std::size_t NoItemIndex = static_cast<std::size_t>(-1);

    [[nodiscard]] DrawPacket MakeRectangle(
        const Rect bounds,
        const Color color)
    {
        DrawPacket packet{};
        packet.type = DrawPacketType::Rectangle;
        packet.bounds = bounds;
        packet.color = color;
        return packet;
    }

    [[nodiscard]] DrawPacket MakeText(const Rect bounds, const Color color, TextContentHandle text,
                                      const Point offset = {})
    {
        DrawPacket packet{};
        packet.type = DrawPacketType::Text;
        packet.bounds = bounds;
        packet.color = color;
        packet.text = std::move(text);
        packet.textOffset = offset;
        return packet;
    }

    [[nodiscard]] Rect NodeRect(const Visual2DNode& node) noexcept
    {
        const Size size = node.NodeSize();
        return {0.0F, 0.0F, size.width, size.height};
    }

    void ValidatePositive(const float value, const char* message)
    {
        if (!std::isfinite(value) || value <= 0.0F)
        {
            throw std::invalid_argument(message);
        }
    }
}

const VisualStyle& SpriteVisualComponent::Style() const noexcept
{
    return style_;
}

void SpriteVisualComponent::SetStyle(const VisualStyle& style) noexcept
{
    style_ = style;
}

void SpriteVisualComponent::SetImage(const ImageHandle image) noexcept
{
    style_.normalImage = image;
    style_.hoveredImage = image;
    style_.pressedImage = image;
    style_.disabledImage = image;
}

void SpriteVisualComponent::SetTint(const Color tint) noexcept
{
    style_.normal = tint;
    style_.hovered = tint;
    style_.pressed = tint;
    style_.disabled = tint;
}

float SpriteVisualComponent::CornerRadius() const noexcept
{
    return cornerRadius_;
}

void SpriteVisualComponent::SetCornerRadius(const float radius)
{
    if (!std::isfinite(radius) || radius < 0.0F)
    {
        throw std::invalid_argument(
            "A Sprite corner radius must be finite and non-negative.");
    }
    cornerRadius_ = radius;
}

DirectX::XMFLOAT2 SpriteVisualComponent::UvScale() const noexcept
{
    return uvScale_;
}

DirectX::XMFLOAT2 SpriteVisualComponent::UvOffset() const noexcept
{
    return uvOffset_;
}

void SpriteVisualComponent::SetUvTransform(
    const DirectX::XMFLOAT2 scale,
    const DirectX::XMFLOAT2 offset) noexcept
{
    uvScale_ = scale;
    uvOffset_ = offset;
}

void SpriteVisualComponent::AppendDrawPackets(
    std::vector<DrawPacket>& packets) const
{
    const Rect bounds = NodeRect(Owner());
    if (bounds.width <= 0.0F || bounds.height <= 0.0F)
    {
        return;
    }
    const Color color = CurrentColor();
    if (const ImageHandle image = CurrentImage(); image)
    {
        DrawPacket packet{};
        packet.type = DrawPacketType::Image;
        packet.bounds = bounds;
        packet.color = color;
        packet.image = image;
        packet.uvScale = uvScale_;
        packet.uvOffset = uvOffset_;
        packet.cornerRadius = cornerRadius_;
        packets.push_back(std::move(packet));
    }
    else if (color.alpha > 0.0F)
    {
        DrawPacket packet = MakeRectangle(bounds, color);
        packet.cornerRadius = cornerRadius_;
        packets.push_back(std::move(packet));
    }
}

Color SpriteVisualComponent::CurrentColor() const noexcept
{
    if (!Owner().IsEnabled())
    {
        return style_.disabled;
    }
    if (Owner().IsPressed())
    {
        return style_.pressed;
    }
    return Owner().IsHovered() ? style_.hovered : style_.normal;
}

ImageHandle SpriteVisualComponent::CurrentImage() const noexcept
{
    if (!Owner().IsEnabled())
    {
        return style_.disabledImage;
    }
    if (Owner().IsPressed())
    {
        return style_.pressedImage;
    }
    return Owner().IsHovered() ? style_.hoveredImage : style_.normalImage;
}

TextVisualComponent::TextVisualComponent(std::wstring text)
    : text_(std::move(text))
{
}

std::wstring_view TextVisualComponent::Text() const noexcept
{
    return text_;
}

void TextVisualComponent::SetText(std::wstring text)
{
    if (text_ == text)
        return;
    text_ = std::move(text);
    content_.reset();
}

const TextFont& TextVisualComponent::Font() const noexcept
{
    return font_;
}

void TextVisualComponent::SetFont(TextFont font)
{
    if (font.source != TextFontSource::Default && font.value.empty())
    {
        throw std::invalid_argument(
            "Visual2D system and file fonts require a name or path.");
    }
    if (font_.source == font.source && font_.value == font.value)
        return;
    font_ = std::move(font);
    content_.reset();
}

float TextVisualComponent::FontSize() const noexcept
{
    return fontSize_;
}

void TextVisualComponent::SetFontSize(const float fontSize)
{
    ValidatePositive(fontSize, "Visual2D font size must be positive.");
    if (fontSize_ == fontSize)
        return;
    fontSize_ = fontSize;
    content_.reset();
}

Color TextVisualComponent::TextColor() const noexcept
{
    return textColor_;
}

std::uint16_t TextVisualComponent::FontWeight() const noexcept
{
    return fontWeight_;
}

void TextVisualComponent::SetFontWeight(const std::uint16_t weight)
{
    if (weight < 1 || weight > 999)
        throw std::invalid_argument("Visual2D font weight must be in 1..999.");
    if (fontWeight_ == weight)
        return;
    fontWeight_ = weight;
    content_.reset();
}

void TextVisualComponent::SetTextColor(const Color color) noexcept
{
    textColor_ = color;
}

TextAlignment TextVisualComponent::HorizontalAlignment() const noexcept
{
    return alignment_;
}

void TextVisualComponent::SetHorizontalAlignment(
    const TextAlignment alignment) noexcept
{
    if (alignment_ == alignment)
        return;
    alignment_ = alignment;
    content_.reset();
}

Rect TextVisualComponent::ContentBounds() const noexcept
{
    return contentBounds_.value_or(NodeRect(Owner()));
}

void TextVisualComponent::SetContentBounds(const Rect bounds)
{
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) ||
        !std::isfinite(bounds.width) || !std::isfinite(bounds.height) ||
        bounds.width < 0.0F || bounds.height < 0.0F)
    {
        throw std::invalid_argument(
            "Visual2D text bounds must be finite and non-negative.");
    }
    contentBounds_ = bounds;
}

void TextVisualComponent::AppendDrawPackets(
    std::vector<DrawPacket>& packets) const
{
    if (!text_.empty())
    {
        if (!content_)
            content_ =
                std::make_shared<TextContent>(TextContent{text_, font_, fontSize_, alignment_, {}, fontWeight_});
        packets.push_back(MakeText(ContentBounds(),
                                   {textColor_.red, textColor_.green, textColor_.blue,
                                    Owner().IsEnabled() ? textColor_.alpha : textColor_.alpha * 0.55F},
                                   content_, textOffset_));
        if (repeatOffset_)
        {
            auto repeat = packets.back();
            repeat.textOffset.x += repeatOffset_->x;
            repeat.textOffset.y += repeatOffset_->y;
            packets.push_back(std::move(repeat));
        }
    }
}

std::optional<Size> TextVisualComponent::MeasuredSize() const noexcept
{
    return content_ ? content_->measuredSize : std::nullopt;
}

void TextVisualComponent::SetTextOffset(Point offset)
{
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y))
        throw std::invalid_argument("Visual2D text offset must be finite.");
    textOffset_ = offset;
}

void TextVisualComponent::SetRepeatOffset(std::optional<Point> offset)
{
    if (offset && (!std::isfinite(offset->x) || !std::isfinite(offset->y)))
        throw std::invalid_argument("Visual2D repeat offset must be finite.");
    repeatOffset_ = offset;
}

RectangleCollider2DComponent::RectangleCollider2DComponent(
    const Rect localBounds)
{
    SetLocalBounds(localBounds);
}

void RectangleCollider2DComponent::SetLocalBounds(const Rect localBounds)
{
    if (!std::isfinite(localBounds.x) || !std::isfinite(localBounds.y) ||
        !std::isfinite(localBounds.width) ||
        !std::isfinite(localBounds.height) ||
        localBounds.width < 0.0F || localBounds.height < 0.0F)
    {
        throw std::invalid_argument(
            "A Visual2D collider requires finite non-negative bounds.");
    }
    localBounds_ = localBounds;
}

void RectangleCollider2DComponent::UseNodeBounds() noexcept
{
    localBounds_.reset();
}

bool RectangleCollider2DComponent::HitTest(
    const Point localPosition) const noexcept
{
    return localBounds_.value_or(NodeRect(Owner())).Contains(localPosition);
}

CircleCollider2DComponent::CircleCollider2DComponent(
    const Point center,
    const float radius)
{
    SetCircle(center, radius);
}

Point CircleCollider2DComponent::Center() const noexcept
{
    return center_;
}

float CircleCollider2DComponent::Radius() const noexcept
{
    return radius_;
}

void CircleCollider2DComponent::SetCircle(
    const Point center,
    const float radius)
{
    if (!std::isfinite(center.x) || !std::isfinite(center.y) ||
        !std::isfinite(radius) || radius < 0.0F)
    {
        throw std::invalid_argument(
            "A Visual2D circle collider requires finite values.");
    }
    center_ = center;
    radius_ = radius;
}

bool CircleCollider2DComponent::HitTest(
    const Point localPosition) const noexcept
{
    const float x = localPosition.x - center_.x;
    const float y = localPosition.y - center_.y;
    return x * x + y * y <= radius_ * radius_;
}

CustomCollider2DComponent::CustomCollider2DComponent(
    HitTestHandler handler)
    : handler_(std::move(handler))
{
    if (!handler_)
    {
        throw std::invalid_argument(
            "A custom Visual2D collider requires a hit-test strategy.");
    }
}

void CustomCollider2DComponent::SetHandler(HitTestHandler handler)
{
    if (!handler)
    {
        throw std::invalid_argument(
            "A custom Visual2D collider requires a hit-test strategy.");
    }
    handler_ = std::move(handler);
}

bool CustomCollider2DComponent::HitTest(
    const Point localPosition) const noexcept
{
    return handler_ && handler_(Owner(), localPosition);
}

PointerReceiverComponent::PointerReceiverComponent(PointerHandler handler)
    : handler_(std::move(handler))
{
}

void PointerReceiverComponent::SetHandler(PointerHandler handler)
{
    handler_ = std::move(handler);
}

void PointerReceiverComponent::OnPointerEvent(
    const PointerEvent& event,
    std::vector<Action>& actions)
{
    if (handler_)
    {
        handler_(Owner(), event, actions);
    }
}

AnimatorComponent::AnimatorComponent(AnimationHandler handler)
    : handler_(std::move(handler))
{
}

void AnimatorComponent::SetHandler(AnimationHandler handler)
{
    handler_ = std::move(handler);
}

void AnimatorComponent::Update(const double elapsedSeconds)
{
    if (handler_)
    {
        handler_(Owner(), elapsedSeconds);
    }
}

void ButtonBehaviorComponent::OnPointerEvent(
    const PointerEvent& event,
    std::vector<Action>& actions)
{
    if (event.type == PointerEventType::Click)
    {
        actions.push_back(Action{
            ActionType::Clicked,
            Owner().Id(),
            0.0F,
            0,
            event.timestampTicks});
    }
}

ToggleBehaviorComponent::ToggleBehaviorComponent(const bool checked)
    : checked_(checked)
{
}

bool ToggleBehaviorComponent::IsChecked() const noexcept
{
    return checked_;
}

void ToggleBehaviorComponent::SetChecked(const bool checked) noexcept
{
    checked_ = checked;
}

void ToggleBehaviorComponent::AppendDrawPackets(
    std::vector<DrawPacket>& packets) const
{
    const Size size = Owner().NodeSize();
    const float indicatorSize = std::min(size.height * 0.5F, 18.0F);
    packets.push_back(MakeRectangle(
        {10.0F,
            (size.height - indicatorSize) * 0.5F,
            indicatorSize,
            indicatorSize},
        checked_
            ? Color{0.20F, 0.85F, 0.48F, 1.0F}
            : Color{0.08F, 0.09F, 0.12F, 1.0F}));
}

void ToggleBehaviorComponent::OnPointerEvent(
    const PointerEvent& event,
    std::vector<Action>& actions)
{
    if (event.type != PointerEventType::Click)
    {
        return;
    }
    checked_ = !checked_;
    actions.push_back(Action{
        ActionType::ValueChanged,
        Owner().Id(),
        checked_ ? 1.0F : 0.0F,
        0,
        event.timestampTicks});
}

SliderBehaviorComponent::SliderBehaviorComponent(const float value)
{
    SetValue(value);
}

float SliderBehaviorComponent::Value() const noexcept
{
    return value_;
}

void SliderBehaviorComponent::SetValue(const float value) noexcept
{
    value_ = std::clamp(value, 0.0F, 1.0F);
}

void SliderBehaviorComponent::AppendDrawPackets(
    std::vector<DrawPacket>& packets) const
{
    const Size size = Owner().NodeSize();
    const float trackHeight = std::min(6.0F, size.height);
    const Rect track{
        8.0F,
        (size.height - trackHeight) * 0.5F,
        std::max(0.0F, size.width - 16.0F),
        trackHeight};
    packets.push_back(MakeRectangle(
        track,
        {0.08F, 0.09F, 0.12F, 1.0F}));
    constexpr float thumbWidth = 12.0F;
    packets.push_back(MakeRectangle(
        {track.x + track.width * value_ - thumbWidth * 0.5F,
            5.0F,
            thumbWidth,
            std::max(0.0F, size.height - 10.0F)},
        {0.30F, 0.70F, 1.0F, 1.0F}));
}

void SliderBehaviorComponent::OnPointerEvent(
    const PointerEvent& event,
    std::vector<Action>& actions)
{
    if (event.type == PointerEventType::Press ||
        (event.type == PointerEventType::Move && Owner().IsPressed()))
    {
        UpdateFromPointer(event, actions);
    }
}

void SliderBehaviorComponent::UpdateFromPointer(
    const PointerEvent& event,
    std::vector<Action>& actions)
{
    const float usableWidth = std::max(Owner().NodeSize().width - 16.0F, 1.0F);
    const float updated = std::clamp(
        (event.localPosition.x - 8.0F) / usableWidth,
        0.0F,
        1.0F);
    if (updated == value_)
    {
        return;
    }
    value_ = updated;
    actions.push_back(Action{
        ActionType::ValueChanged,
        Owner().Id(),
        value_,
        0,
        event.timestampTicks});
}

void CycleSelectorBehaviorComponent::SetItems(
    std::vector<std::wstring> items)
{
    items_ = std::move(items);
    if (selectedIndex_ >= items_.size())
    {
        selectedIndex_ = 0;
    }
    RefreshText();
}

const std::vector<std::wstring>&
CycleSelectorBehaviorComponent::Items() const noexcept
{
    return items_;
}

std::size_t CycleSelectorBehaviorComponent::SelectedIndex() const noexcept
{
    return selectedIndex_;
}

void CycleSelectorBehaviorComponent::SetSelectedIndex(
    const std::size_t index)
{
    if (index >= items_.size())
    {
        throw std::out_of_range(
            "Visual2D cycle-selector selection is out of range.");
    }
    selectedIndex_ = index;
    RefreshText();
}

void CycleSelectorBehaviorComponent::OnPointerEvent(
    const PointerEvent& event,
    std::vector<Action>& actions)
{
    if (event.type != PointerEventType::Click || items_.empty())
    {
        return;
    }
    selectedIndex_ = (selectedIndex_ + 1) % items_.size();
    RefreshText();
    actions.push_back(Action{
        ActionType::SelectionChanged,
        Owner().Id(),
        static_cast<float>(selectedIndex_),
        selectedIndex_,
        event.timestampTicks});
}

void CycleSelectorBehaviorComponent::RefreshText()
{
    if (TextVisualComponent* text = Owner().GetComponent<TextVisualComponent>())
    {
        text->SetText(items_.empty() ? std::wstring{} : items_[selectedIndex_]);
    }
}

void ComboBoxBehaviorComponent::SetItems(std::vector<std::wstring> items)
{
    items_ = std::move(items);
    itemContent_.clear();
    if (selectedIndex_ >= items_.size())
    {
        selectedIndex_ = 0;
    }
    firstVisibleIndex_ = 0;
    hoveredItemIndex_ = NoItemIndex;
    if (items_.empty())
    {
        Collapse();
    }
}

const std::vector<std::wstring>&
ComboBoxBehaviorComponent::Items() const noexcept
{
    return items_;
}

std::size_t ComboBoxBehaviorComponent::SelectedIndex() const noexcept
{
    return selectedIndex_;
}

void ComboBoxBehaviorComponent::SetSelectedIndex(const std::size_t index)
{
    if (index >= items_.size())
    {
        throw std::out_of_range(
            "Visual2D combo-box selection is out of range.");
    }
    selectedIndex_ = index;
    EnsureSelectedItemVisible();
}

void ComboBoxBehaviorComponent::SetMaxVisibleItems(
    const std::size_t maxVisibleItems)
{
    if (maxVisibleItems == 0)
    {
        throw std::invalid_argument(
            "A Visual2D combo box must show at least one item.");
    }
    maxVisibleItems_ = maxVisibleItems;
    EnsureSelectedItemVisible();
}

std::size_t ComboBoxBehaviorComponent::MaxVisibleItems() const noexcept
{
    return maxVisibleItems_;
}

void ComboBoxBehaviorComponent::SetItemHeight(const float itemHeight)
{
    ValidatePositive(
        itemHeight,
        "A Visual2D combo-box item height must be positive.");
    itemHeight_ = itemHeight;
}

float ComboBoxBehaviorComponent::ItemHeight() const noexcept
{
    return itemHeight_;
}

const TextFont& ComboBoxBehaviorComponent::Font() const noexcept
{
    return font_;
}

void ComboBoxBehaviorComponent::SetFont(TextFont font)
{
    if (font.source != TextFontSource::Default && font.value.empty())
    {
        throw std::invalid_argument(
            "Visual2D system and file fonts require a name or path.");
    }
    if (font_.source == font.source && font_.value == font.value)
        return;
    font_ = std::move(font);
    itemContent_.clear();
    arrowContent_ = {};
}

void ComboBoxBehaviorComponent::SetFontSize(const float fontSize)
{
    ValidatePositive(fontSize, "Visual2D font size must be positive.");
    if (fontSize_ == fontSize)
        return;
    fontSize_ = fontSize;
    itemContent_.clear();
    arrowContent_ = {};
}

Color ComboBoxBehaviorComponent::TextColor() const noexcept
{
    return textColor_;
}

void ComboBoxBehaviorComponent::SetTextColor(const Color color) noexcept
{
    textColor_ = color;
}

Color ComboBoxBehaviorComponent::SelectedTextColor() const noexcept
{
    return selectedTextColor_;
}

void ComboBoxBehaviorComponent::SetSelectedTextColor(
    const Color color) noexcept
{
    selectedTextColor_ = color;
}

Color ComboBoxBehaviorComponent::PopupBackgroundColor() const noexcept
{
    return popupBackgroundColor_;
}

void ComboBoxBehaviorComponent::SetPopupBackgroundColor(
    const Color color) noexcept
{
    popupBackgroundColor_ = color;
}

bool ComboBoxBehaviorComponent::IsExpanded() const noexcept
{
    return expanded_;
}

void ComboBoxBehaviorComponent::Collapse() noexcept
{
    expanded_ = false;
    trackingDrag_ = false;
    dragMoved_ = false;
    hoveredItemIndex_ = NoItemIndex;
}

void ComboBoxBehaviorComponent::AppendDrawPackets(
    std::vector<DrawPacket>& packets) const
{
    EnsureTextContent();
    const Size size = Owner().NodeSize();
    const float arrowWidth = std::min(34.0F, size.width);
    if (!items_.empty())
    {
        packets.push_back(MakeText({12.0F, 0.0F, std::max(size.width - arrowWidth - 18.0F, 0.0F), size.height},
                                   {textColor_.red, textColor_.green, textColor_.blue,
                                    Owner().IsEnabled() ? textColor_.alpha : textColor_.alpha * 0.55F},
                                   itemContent_[selectedIndex_]));
    }
    packets.push_back(MakeRectangle(
        {size.width - arrowWidth, 0.0F, arrowWidth, size.height},
        {0.025F, 0.055F, 0.125F, Owner().IsEnabled() ? 0.92F : 0.50F}));
    packets.push_back(MakeText({size.width - arrowWidth, 0.0F, arrowWidth, size.height},
                               {0.72F, 0.84F, 1.0F, Owner().IsEnabled() ? 1.0F : 0.55F},
                               arrowContent_[expanded_ ? 1 : 0]));

    if (!expanded_ || items_.empty())
    {
        return;
    }
    const Rect popup = PopupBounds();
    packets.push_back(MakeRectangle(
        popup,
        popupBackgroundColor_));
    const std::size_t visibleCount = VisibleItemCount();
    for (std::size_t row = 0; row < visibleCount; ++row)
    {
        const std::size_t itemIndex = firstVisibleIndex_ + row;
        const Rect itemBounds{
            popup.x,
            popup.y + popup.height -
                static_cast<float>(row + 1) * itemHeight_,
            popup.width,
            itemHeight_};
        const VisualStyle& style = OwnerStyle();
        const Color itemColor = itemIndex == selectedIndex_
            ? style.pressed
            : itemIndex == hoveredItemIndex_ ? style.hovered : style.normal;
        packets.push_back(MakeRectangle(itemBounds, itemColor));
        packets.push_back(MakeText(
            {itemBounds.x + 10.0F, itemBounds.y, std::max(itemBounds.width - 20.0F, 0.0F), itemBounds.height},
            itemIndex == selectedIndex_ ? selectedTextColor_ : textColor_, itemContent_[itemIndex]));
    }

    if (items_.size() > visibleCount && popup.height > 0.0F)
    {
        constexpr float trackWidth = 6.0F;
        const float thumbHeight = std::max(
            12.0F,
            popup.height * static_cast<float>(visibleCount) /
                static_cast<float>(items_.size()));
        const std::size_t maxFirstIndex = items_.size() - visibleCount;
        const float progress = maxFirstIndex == 0
            ? 0.0F
            : static_cast<float>(firstVisibleIndex_) /
                static_cast<float>(maxFirstIndex);
        packets.push_back(MakeRectangle(
            {popup.x + popup.width - trackWidth,
                popup.y,
                trackWidth,
                popup.height},
            {0.02F, 0.025F, 0.04F, 0.90F}));
        packets.push_back(MakeRectangle(
            {popup.x + popup.width - trackWidth,
                popup.y +
                    (popup.height - thumbHeight) * (1.0F - progress),
                trackWidth,
                thumbHeight},
            {0.35F, 0.70F, 1.0F, 1.0F}));
    }
}

void ComboBoxBehaviorComponent::EnsureTextContent() const
{
    if (itemContent_.size() != items_.size())
        itemContent_.resize(items_.size());
    const auto prepareItem = [this](std::size_t index)
    {
        if (!itemContent_[index])
            itemContent_[index] = std::make_shared<TextContent>(
                TextContent{items_[index], font_, fontSize_, TextAlignment::Leading, {}});
    };
    if (!items_.empty())
        prepareItem(selectedIndex_);
    if (expanded_)
        for (std::size_t row = 0; row < VisibleItemCount(); ++row)
            prepareItem(firstVisibleIndex_ + row);
    if (!arrowContent_[0])
    {
        arrowContent_[0] = std::make_shared<TextContent>(
            TextContent{L"▼", font_, std::min(fontSize_, 14.0F), TextAlignment::Center, {}});
        arrowContent_[1] = std::make_shared<TextContent>(
            TextContent{L"▲", font_, std::min(fontSize_, 14.0F), TextAlignment::Center, {}});
    }
}

bool ComboBoxBehaviorComponent::HitTest(
    const Point localPosition) const noexcept
{
    return NodeRect(Owner()).Contains(localPosition) ||
        (expanded_ && PopupBounds().Contains(localPosition));
}

void ComboBoxBehaviorComponent::OnPointerEvent(
    const PointerEvent& event,
    std::vector<Action>& actions)
{
    if (items_.empty())
    {
        return;
    }
    if (event.type == PointerEventType::Enter ||
        event.type == PointerEventType::Move)
    {
        if (expanded_)
        {
            UpdateHoveredItem(event.localPosition);
        }
        if (event.type == PointerEventType::Move && trackingDrag_ &&
            Owner().IsPressed())
        {
            const float displacement = event.localPosition.y - dragStartY_;
            if (std::abs(displacement) > 3.0F)
            {
                const int itemDelta = static_cast<int>(
                    std::trunc(displacement / itemHeight_));
                ScrollBy(static_cast<int>(dragStartFirstVisibleIndex_) +
                    itemDelta - static_cast<int>(firstVisibleIndex_));
                dragMoved_ = true;
            }
        }
        return;
    }
    if (event.type == PointerEventType::Leave)
    {
        hoveredItemIndex_ = NoItemIndex;
        return;
    }
    if (event.type == PointerEventType::Wheel && expanded_ &&
        PopupBounds().Contains(event.localPosition))
    {
        const int direction = event.wheelDelta > 0.0F ? -1 : 1;
        const int steps = std::max(
            1,
            static_cast<int>(std::round(std::abs(event.wheelDelta))));
        ScrollBy(direction * steps);
        UpdateHoveredItem(event.localPosition);
        return;
    }
    if (event.type == PointerEventType::Press && expanded_ &&
        PopupBounds().Contains(event.localPosition))
    {
        trackingDrag_ = true;
        dragMoved_ = false;
        dragStartY_ = event.localPosition.y;
        dragStartFirstVisibleIndex_ = firstVisibleIndex_;
        return;
    }
    if (event.type == PointerEventType::Release)
    {
        trackingDrag_ = false;
        return;
    }
    if (event.type != PointerEventType::Click)
    {
        return;
    }
    if (!expanded_)
    {
        expanded_ = true;
        EnsureSelectedItemVisible();
        UpdateHoveredItem(event.localPosition);
        return;
    }
    if (dragMoved_)
    {
        dragMoved_ = false;
        return;
    }
    if (const std::optional<std::size_t> selected =
            ItemIndexAt(event.localPosition);
        selected.has_value())
    {
        selectedIndex_ = *selected;
        Collapse();
        actions.push_back(Action{
            ActionType::SelectionChanged,
            Owner().Id(),
            static_cast<float>(selectedIndex_),
            selectedIndex_,
            event.timestampTicks});
        return;
    }
    if (NodeRect(Owner()).Contains(event.localPosition))
    {
        Collapse();
    }
}

Rect ComboBoxBehaviorComponent::PopupBounds() const noexcept
{
    const Size size = Owner().NodeSize();
    const float popupHeight =
        itemHeight_ * static_cast<float>(VisibleItemCount());
    return {0.0F, -popupHeight, size.width, popupHeight};
}

std::size_t ComboBoxBehaviorComponent::VisibleItemCount() const noexcept
{
    return std::min(items_.size(), maxVisibleItems_);
}

std::optional<std::size_t> ComboBoxBehaviorComponent::ItemIndexAt(
    const Point localPosition) const noexcept
{
    const Rect popup = PopupBounds();
    if (!popup.Contains(localPosition) || itemHeight_ <= 0.0F)
    {
        return std::nullopt;
    }
    const std::size_t row = static_cast<std::size_t>(
        (popup.y + popup.height - localPosition.y) / itemHeight_);
    const std::size_t index = firstVisibleIndex_ + row;
    return row < VisibleItemCount() && index < items_.size()
        ? std::optional<std::size_t>{index}
        : std::nullopt;
}

void ComboBoxBehaviorComponent::EnsureSelectedItemVisible() noexcept
{
    const std::size_t visibleCount = VisibleItemCount();
    if (visibleCount == 0)
    {
        firstVisibleIndex_ = 0;
        return;
    }
    if (selectedIndex_ < firstVisibleIndex_)
    {
        firstVisibleIndex_ = selectedIndex_;
    }
    else if (selectedIndex_ >= firstVisibleIndex_ + visibleCount)
    {
        firstVisibleIndex_ = selectedIndex_ - visibleCount + 1;
    }
    firstVisibleIndex_ = std::min(
        firstVisibleIndex_,
        items_.size() - visibleCount);
}

void ComboBoxBehaviorComponent::ScrollBy(const int itemDelta) noexcept
{
    const std::size_t visibleCount = VisibleItemCount();
    if (visibleCount == 0 || items_.size() <= visibleCount)
    {
        return;
    }
    const int maximum = static_cast<int>(items_.size() - visibleCount);
    firstVisibleIndex_ = static_cast<std::size_t>(std::clamp(
        static_cast<int>(firstVisibleIndex_) + itemDelta,
        0,
        maximum));
}

void ComboBoxBehaviorComponent::UpdateHoveredItem(
    const Point localPosition) noexcept
{
    hoveredItemIndex_ = ItemIndexAt(localPosition).value_or(NoItemIndex);
}

const VisualStyle& ComboBoxBehaviorComponent::OwnerStyle() const noexcept
{
    static const VisualStyle fallback{};
    if (const auto* sprite = Owner().GetComponent<SpriteVisualComponent>())
    {
        return sprite->Style();
    }
    return fallback;
}

} // namespace PhantomCore
