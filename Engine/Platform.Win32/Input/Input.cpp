#include "Input/Input.h"

#include <algorithm>
#include <new>
#include <stdexcept>


namespace PhantomCore
{
namespace
{
    constexpr std::size_t ToIndex(const MouseButton button) noexcept
    {
        return static_cast<std::size_t>(button);
    }
}

InputState::InputState()
{
    events_.reserve(256);
}

bool InputState::IsKeyDown(const std::uint16_t virtualKey) const noexcept
{
    return virtualKey < KeyCount && keysDown_[virtualKey];
}

bool InputState::WasKeyPressed(
    const std::uint16_t virtualKey) const noexcept
{
    return virtualKey < KeyCount && keysPressed_[virtualKey];
}

bool InputState::WasKeyReleased(
    const std::uint16_t virtualKey) const noexcept
{
    return virtualKey < KeyCount && keysReleased_[virtualKey];
}

bool InputState::IsMouseButtonDown(const MouseButton button) const noexcept
{
    return mouseButtonsDown_[ToIndex(button)];
}

bool InputState::WasMouseButtonPressed(const MouseButton button) const noexcept
{
    return mouseButtonsPressed_[ToIndex(button)];
}

bool InputState::WasMouseButtonReleased(const MouseButton button) const noexcept
{
    return mouseButtonsReleased_[ToIndex(button)];
}

std::int32_t InputState::MouseDeltaX() const noexcept
{
    return mouseDeltaX_;
}

std::int32_t InputState::MouseDeltaY() const noexcept
{
    return mouseDeltaY_;
}

std::int32_t InputState::MousePositionX() const noexcept
{
    return mousePositionX_;
}

std::int32_t InputState::MousePositionY() const noexcept
{
    return mousePositionY_;
}

bool InputState::IsMouseInsideWindow() const noexcept
{
    return mouseInsideWindow_;
}

float InputState::MouseWheelDelta() const noexcept
{
    return mouseWheelDelta_;
}

std::span<const InputEvent> InputState::Events() const noexcept
{
    return events_;
}

std::int64_t InputState::PerformanceCounterFrequency() const noexcept
{
    return performanceCounterFrequency_;
}

void InputState::BeginUpdate() noexcept
{
    std::ranges::fill(keysPressed_, false);
    std::ranges::fill(keysReleased_, false);
    std::ranges::fill(mouseButtonsPressed_, false);
    std::ranges::fill(mouseButtonsReleased_, false);
    mouseDeltaX_ = 0;
    mouseDeltaY_ = 0;
    mouseInsideWindow_ = false;
    mouseWheelDelta_ = 0.0F;
    events_.clear();
    eventStorageFailed_ = false;
}

void InputState::Reset() noexcept
{
    std::ranges::fill(keysDown_, false);
    std::ranges::fill(keysPressed_, false);
    std::ranges::fill(keysReleased_, false);
    std::ranges::fill(mouseButtonsDown_, false);
    std::ranges::fill(mouseButtonsPressed_, false);
    std::ranges::fill(mouseButtonsReleased_, false);
    mouseDeltaX_ = 0;
    mouseDeltaY_ = 0;
    mouseInsideWindow_ = false;
    mouseWheelDelta_ = 0.0F;
    events_.clear();
    eventStorageFailed_ = false;
}

void InputState::SetPerformanceCounterFrequency(
    const std::int64_t frequency) noexcept
{
    performanceCounterFrequency_ = frequency;
}

void InputState::SetKey(
    const std::uint16_t virtualKey,
    const bool isDown,
    const std::int64_t timestamp) noexcept
{
    if (virtualKey >= KeyCount)
    {
        return;
    }

    const bool wasDown = keysDown_[virtualKey];
    keysDown_[virtualKey] = isDown;
    keysPressed_[virtualKey] = keysPressed_[virtualKey] || (isDown && !wasDown);
    keysReleased_[virtualKey] = keysReleased_[virtualKey] || (!isDown && wasDown);

    if (isDown != wasDown)
    {
        PushEvent(InputEvent{
            isDown
                ? InputEventType::KeyPressed
                : InputEventType::KeyReleased,
            virtualKey,
            0,
            0,
            0.0F,
            timestamp});
    }
}

void InputState::SetMouseButton(
    const MouseButton button,
    const bool isDown,
    const std::int64_t timestamp) noexcept
{
    const std::size_t index = ToIndex(button);
    const bool wasDown = mouseButtonsDown_[index];
    mouseButtonsDown_[index] = isDown;
    mouseButtonsPressed_[index] =
        mouseButtonsPressed_[index] || (isDown && !wasDown);
    mouseButtonsReleased_[index] =
        mouseButtonsReleased_[index] || (!isDown && wasDown);

    if (isDown != wasDown)
    {
        PushEvent(InputEvent{
            isDown
                ? InputEventType::MouseButtonPressed
                : InputEventType::MouseButtonReleased,
            static_cast<std::uint16_t>(button),
            0,
            0,
            0.0F,
            timestamp});
    }
}

void InputState::AddMouseDelta(
    const std::int32_t x,
    const std::int32_t y,
    const std::int64_t timestamp) noexcept
{
    mouseDeltaX_ += x;
    mouseDeltaY_ += y;
    if (x != 0 || y != 0)
    {
        PushEvent(InputEvent{
            InputEventType::MouseMoved,
            0,
            x,
            y,
            0.0F,
            timestamp});
    }
}

void InputState::AddMouseWheel(
    const float delta,
    const std::int64_t timestamp) noexcept
{
    mouseWheelDelta_ += delta;
    PushEvent(InputEvent{
        InputEventType::MouseWheel,
        0,
        0,
        0,
        delta,
        timestamp});
}

void InputState::SetMousePosition(
    const std::int32_t x,
    const std::int32_t y,
    const bool insideWindow) noexcept
{
    mousePositionX_ = x;
    mousePositionY_ = y;
    mouseInsideWindow_ = insideWindow;
}

void InputState::PushEvent(const InputEvent& event) noexcept
{
    if (eventStorageFailed_)
        return;
    // Never unwind through the Win32 callback. PumpMessages reports a
    // storage failure before an incomplete stream can reach the Client.
    try
    {
        events_.push_back(event);
    }
    catch (const std::bad_alloc &)
    {
        eventStorageFailed_ = true;
    }
    catch (const std::length_error &)
    {
        eventStorageFailed_ = true;
    }
}

} // namespace PhantomCore
