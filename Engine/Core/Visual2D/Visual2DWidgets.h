#pragma once

#include "Visual2D/Visual2DCanvas.h"
#include "Visual2D/Visual2DComponents.h"


// Widget factories assemble reusable component combinations. The returned
// object is still a regular Visual2DNode and can gain or lose components.
namespace PhantomCore
{
[[nodiscard]] Visual2DNode& CreateSprite(
    Visual2DNode& parent,
    Rect bounds,
    ImageHandle image = {},
    std::string name = "Sprite");
[[nodiscard]] Visual2DNode& CreatePanel(
    Visual2DNode& parent,
    Rect bounds,
    std::string name = "Panel");
[[nodiscard]] Visual2DNode& CreateLabel(
    Visual2DNode& parent,
    Rect bounds,
    std::wstring text,
    std::string name = "Label");
[[nodiscard]] Visual2DNode& CreateButton(
    Visual2DNode& parent,
    Rect bounds,
    std::wstring text,
    std::string name = "Button");
[[nodiscard]] Visual2DNode& CreateToggle(
    Visual2DNode& parent,
    Rect bounds,
    std::wstring text,
    bool checked = false,
    std::string name = "Toggle");
[[nodiscard]] Visual2DNode& CreateSlider(
    Visual2DNode& parent,
    Rect bounds,
    float value = 0.0F,
    std::string name = "Slider");
[[nodiscard]] Visual2DNode& CreateCycleSelector(
    Visual2DNode& parent,
    Rect bounds,
    std::vector<std::wstring> items,
    std::string name = "CycleSelector");
[[nodiscard]] Visual2DNode& CreateComboBox(
    Visual2DNode& parent,
    Rect bounds,
    std::vector<std::wstring> items,
    std::string name = "ComboBox");

} // namespace PhantomCore
