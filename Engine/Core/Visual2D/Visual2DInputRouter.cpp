#include "Visual2D/Visual2DInputRouter.h"


namespace PhantomCore
{
void Visual2DInputRouter::Process(
    Visual2DCanvas& canvas,
    const PointerInput& input)
{
    const bool pointerChanged =
        !hasPointerSnapshot_ ||
        input.available != previousAvailable_ ||
        (input.available &&
            (input.position.x != previousPosition_.x ||
                input.position.y != previousPosition_.y));
    const bool hasTransientInput =
        input.leftButtonPressed || input.leftButtonReleased ||
        input.wheelDelta != 0.0F;
    const bool refreshHitTest = hitTestInvalidated_ || pointerChanged;

    previousPosition_ = input.position;
    previousAvailable_ = input.available;
    hasPointerSnapshot_ = true;

    // The unlimited Update loop commonly observes the same pointer state
    // thousands of times between OS input messages. Preserve hover and
    // capture without traversing the Canvas or emitting synthetic moves.
    if (!refreshHitTest && !hasTransientInput)
    {
        return;
    }
    hitTestInvalidated_ = false;

    Visual2DNode::HitResult hit{};
    if (input.available)
    {
        hit = canvas.HitTest(input.position);
    }
    ChangeHovered(canvas, hit.node, input);

    if (input.wheelDelta != 0.0F && hit.node != nullptr)
    {
        Dispatch(canvas, *hit.node, PointerEventType::Wheel, input);
    }
    if (input.leftButtonPressed && hit.node != nullptr)
    {
        captured_ = hit.node->Id();
        hit.node->SetPressed(true);
        Dispatch(
            canvas,
            *hit.node,
            PointerEventType::Press,
            input,
            PointerButton::Left);
    }

    Visual2DNode* captured = captured_ != 0
        ? canvas.FindNode(captured_)
        : nullptr;
    Visual2DNode* moveTarget = captured != nullptr ? captured : hit.node;
    if (moveTarget != nullptr && refreshHitTest)
    {
        Dispatch(canvas, *moveTarget, PointerEventType::Move, input);
    }

    if (input.leftButtonReleased && captured != nullptr)
    {
        Dispatch(
            canvas,
            *captured,
            PointerEventType::Release,
            input,
            PointerButton::Left);
        captured->SetPressed(false);
        if (hit.node == captured)
        {
            Dispatch(
                canvas,
                *captured,
                PointerEventType::Click,
                input,
                PointerButton::Left);
        }
        captured_ = 0;
    }
    else if (!input.leftButtonDown && !input.leftButtonPressed)
    {
        if (captured != nullptr)
        {
            captured->SetPressed(false);
        }
        captured_ = 0;
    }
}

void Visual2DInputRouter::Reset(Visual2DCanvas& canvas) noexcept
{
    if (Visual2DNode* hovered = canvas.FindNode(hovered_))
    {
        hovered->SetHovered(false);
    }
    if (Visual2DNode* captured = canvas.FindNode(captured_))
    {
        captured->SetPressed(false);
    }
    hovered_ = 0;
    captured_ = 0;
    previousPosition_ = {};
    previousAvailable_ = false;
    hasPointerSnapshot_ = false;
    hitTestInvalidated_ = true;
}

void Visual2DInputRouter::InvalidateHitTest() noexcept
{
    hitTestInvalidated_ = true;
}

NodeId Visual2DInputRouter::HoveredNode() const noexcept
{
    return hovered_;
}

NodeId Visual2DInputRouter::CapturedNode() const noexcept
{
    return captured_;
}

void Visual2DInputRouter::Dispatch(
    Visual2DCanvas& canvas,
    Visual2DNode& node,
    const PointerEventType type,
    const PointerInput& input,
    const PointerButton button)
{
    Point local{};
    if (!node.MapCanvasPointToLocal(input.position, local))
    {
        local = {};
    }
    node.DispatchPointerEvent(
        PointerEvent{
            type,
            button,
            input.position,
            local,
            input.timestampTicks,
            input.wheelDelta},
        canvas.actions_);
}

void Visual2DInputRouter::ChangeHovered(
    Visual2DCanvas& canvas,
    Visual2DNode* next,
    const PointerInput& input)
{
    if (next != nullptr && next->Id() == hovered_)
    {
        return;
    }
    if (Visual2DNode* previous = canvas.FindNode(hovered_))
    {
        previous->SetHovered(false);
        Dispatch(canvas, *previous, PointerEventType::Leave, input);
    }
    hovered_ = next != nullptr ? next->Id() : 0;
    if (next != nullptr)
    {
        next->SetHovered(true);
        Dispatch(canvas, *next, PointerEventType::Enter, input);
    }
}

} // namespace PhantomCore
