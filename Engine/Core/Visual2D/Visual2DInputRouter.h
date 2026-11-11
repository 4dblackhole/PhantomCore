#pragma once

#include "Visual2D/Visual2DCanvas.h"


namespace PhantomCore
{
struct PointerInput
{
    Point position{};
    bool available{true};
    bool leftButtonDown{};
    bool leftButtonPressed{};
    bool leftButtonReleased{};
    float wheelDelta{};
    std::int64_t timestampTicks{};
};

// Converts pointer snapshots into hover, capture, release and click events.
class Visual2DInputRouter final
{
public:
    void Process(Visual2DCanvas& canvas, const PointerInput& input);
    void Reset(Visual2DCanvas& canvas) noexcept;
    // Call after moving or resizing interactive Canvas content beneath a
    // stationary pointer. The next Process refreshes hover and hit state.
    void InvalidateHitTest() noexcept;

    [[nodiscard]] NodeId HoveredNode() const noexcept;
    [[nodiscard]] NodeId CapturedNode() const noexcept;

private:
    void Dispatch(
        Visual2DCanvas& canvas,
        Visual2DNode& node,
        PointerEventType type,
        const PointerInput& input,
        PointerButton button = PointerButton::None);
    void ChangeHovered(
        Visual2DCanvas& canvas,
        Visual2DNode* next,
        const PointerInput& input);

    NodeId hovered_{};
    NodeId captured_{};
    Point previousPosition_{};
    bool previousAvailable_{};
    bool hasPointerSnapshot_{};
    bool hitTestInvalidated_{true};
};

} // namespace PhantomCore
