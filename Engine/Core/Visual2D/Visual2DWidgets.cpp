#include "Visual2D/Visual2DWidgets.h"


namespace PhantomCore
{
namespace
{
    [[nodiscard]] VisualStyle TransparentStyle() noexcept
    {
        VisualStyle style{};
        style.normal.alpha = 0.0F;
        style.hovered.alpha = 0.0F;
        style.pressed.alpha = 0.0F;
        style.disabled.alpha = 0.0F;
        return style;
    }

    void AddPointerComponents(Visual2DNode& node)
    {
        node.AddComponent<RectangleCollider2DComponent>();
        node.AddComponent<PointerReceiverComponent>();
    }
}

Visual2DNode& CreateSprite(
    Visual2DNode& parent,
    const Rect bounds,
    const ImageHandle image,
    std::string name)
{
    Visual2DNode& node = parent.CreateChild(std::move(name));
    node.SetBounds(bounds);
    SpriteVisualComponent& sprite =
        node.AddComponent<SpriteVisualComponent>();
    if (image)
    {
        sprite.SetImage(image);
        sprite.SetTint({1.0F, 1.0F, 1.0F, 1.0F});
    }
    return node;
}

Visual2DNode& CreatePanel(
    Visual2DNode& parent,
    const Rect bounds,
    std::string name)
{
    return CreateSprite(parent, bounds, {}, std::move(name));
}

Visual2DNode& CreateLabel(
    Visual2DNode& parent,
    const Rect bounds,
    std::wstring text,
    std::string name)
{
    Visual2DNode& node = CreatePanel(parent, bounds, std::move(name));
    node.GetComponent<SpriteVisualComponent>()->SetStyle(
        TransparentStyle());
    node.AddComponent<TextVisualComponent>(std::move(text));
    return node;
}

Visual2DNode& CreateButton(
    Visual2DNode& parent,
    const Rect bounds,
    std::wstring text,
    std::string name)
{
    Visual2DNode& node = CreatePanel(parent, bounds, std::move(name));
    TextVisualComponent& label =
        node.AddComponent<TextVisualComponent>(std::move(text));
    label.SetHorizontalAlignment(TextAlignment::Center);
    AddPointerComponents(node);
    node.AddComponent<ButtonBehaviorComponent>();
    return node;
}

Visual2DNode& CreateToggle(
    Visual2DNode& parent,
    const Rect bounds,
    std::wstring text,
    const bool checked,
    std::string name)
{
    Visual2DNode& node = CreatePanel(parent, bounds, std::move(name));
    TextVisualComponent& label =
        node.AddComponent<TextVisualComponent>(std::move(text));
    label.SetHorizontalAlignment(TextAlignment::Center);
    AddPointerComponents(node);
    node.AddComponent<ToggleBehaviorComponent>(checked);
    return node;
}

Visual2DNode& CreateSlider(
    Visual2DNode& parent,
    const Rect bounds,
    const float value,
    std::string name)
{
    Visual2DNode& node = CreatePanel(parent, bounds, std::move(name));
    AddPointerComponents(node);
    node.AddComponent<SliderBehaviorComponent>(value);
    return node;
}

Visual2DNode& CreateCycleSelector(
    Visual2DNode& parent,
    const Rect bounds,
    std::vector<std::wstring> items,
    std::string name)
{
    Visual2DNode& node = CreatePanel(parent, bounds, std::move(name));
    TextVisualComponent& label = node.AddComponent<TextVisualComponent>();
    label.SetHorizontalAlignment(TextAlignment::Center);
    AddPointerComponents(node);
    CycleSelectorBehaviorComponent& behavior =
        node.AddComponent<CycleSelectorBehaviorComponent>();
    behavior.SetItems(std::move(items));
    return node;
}

Visual2DNode& CreateComboBox(
    Visual2DNode& parent,
    const Rect bounds,
    std::vector<std::wstring> items,
    std::string name)
{
    Visual2DNode& node = CreatePanel(parent, bounds, std::move(name));
    node.AddComponent<PointerReceiverComponent>();
    ComboBoxBehaviorComponent& behavior =
        node.AddComponent<ComboBoxBehaviorComponent>();
    behavior.SetItems(std::move(items));
    return node;
}

} // namespace PhantomCore
