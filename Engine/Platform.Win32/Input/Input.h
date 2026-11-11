#pragma once

// Input feature: timestamped Win32 Raw Input state and events.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>


namespace PhantomCore
{
enum class MouseButton : std::uint8_t
{
    Left,
    Right,
    Middle,
    X1,
    X2,
    Count,
};

enum class InputEventType : std::uint8_t
{
    KeyPressed,
    KeyReleased,
    MouseButtonPressed,
    MouseButtonReleased,
    MouseMoved,
    MouseWheel,
};

struct InputEvent
{
    InputEventType type{};
    std::uint16_t code{};
    std::int32_t valueX{};
    std::int32_t valueY{};
    float wheelDelta{};
    std::int64_t performanceCounterTicks{};
};

// Public snapshot and timestamped event stream populated by Win32 Raw Input.
// PumpMessages begins a fresh update turn before dispatching messages:
// pressed/released, mouse deltas, wheel, and Events() are therefore only
// for that turn, while IsKeyDown/IsMouseButtonDown persist as held state.
class InputState final
{
public:
    InputState();

    // Accepts a Win32 Virtual-Key value (for example VK_ESCAPE or 'W').
    // Raw Input keeps left/right modifier keys distinct, so query
    // VK_LSHIFT/VK_RSHIFT, VK_LCONTROL/VK_RCONTROL, or VK_LMENU/VK_RMENU.
    [[nodiscard]] bool IsKeyDown(std::uint16_t virtualKey) const noexcept;
    [[nodiscard]] bool WasKeyPressed(
        std::uint16_t virtualKey) const noexcept;
    [[nodiscard]] bool WasKeyReleased(
        std::uint16_t virtualKey) const noexcept;

    [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const noexcept;
    [[nodiscard]] bool WasMouseButtonPressed(MouseButton button) const noexcept;
    [[nodiscard]] bool WasMouseButtonReleased(MouseButton button) const noexcept;

    [[nodiscard]] std::int32_t MouseDeltaX() const noexcept;
    [[nodiscard]] std::int32_t MouseDeltaY() const noexcept;
    // Absolute Win32 client coordinates are a pointer-placement snapshot.
    // Raw Input events remain the authoritative timestamped input stream.
    [[nodiscard]] std::int32_t MousePositionX() const noexcept;
    [[nodiscard]] std::int32_t MousePositionY() const noexcept;
    [[nodiscard]] bool IsMouseInsideWindow() const noexcept;
    [[nodiscard]] float MouseWheelDelta() const noexcept;
    [[nodiscard]] std::span<const InputEvent> Events() const noexcept;
    [[nodiscard]] std::int64_t PerformanceCounterFrequency() const noexcept;

private:
    friend class Win32Window;
    friend struct InputStateTestAccess;

    static constexpr std::size_t KeyCount = 256;
    static constexpr std::size_t MouseButtonCount =
        static_cast<std::size_t>(MouseButton::Count);

    void BeginUpdate() noexcept;
    void Reset() noexcept;
    void SetPerformanceCounterFrequency(std::int64_t frequency) noexcept;
    void SetKey(
        std::uint16_t virtualKey,
        bool isDown,
        std::int64_t timestamp) noexcept;
    void SetMouseButton(
        MouseButton button,
        bool isDown,
        std::int64_t timestamp) noexcept;
    void AddMouseDelta(
        std::int32_t x,
        std::int32_t y,
        std::int64_t timestamp) noexcept;
    void SetMousePosition(
        std::int32_t x,
        std::int32_t y,
        bool insideWindow) noexcept;
    void AddMouseWheel(float delta, std::int64_t timestamp) noexcept;
    void PushEvent(const InputEvent& event) noexcept;

    std::array<bool, KeyCount> keysDown_{};
    std::array<bool, KeyCount> keysPressed_{};
    std::array<bool, KeyCount> keysReleased_{};
    std::array<bool, MouseButtonCount> mouseButtonsDown_{};
    std::array<bool, MouseButtonCount> mouseButtonsPressed_{};
    std::array<bool, MouseButtonCount> mouseButtonsReleased_{};
    std::int32_t mouseDeltaX_{};
    std::int32_t mouseDeltaY_{};
    std::int32_t mousePositionX_{};
    std::int32_t mousePositionY_{};
    bool mouseInsideWindow_{};
    float mouseWheelDelta_{};
    std::vector<InputEvent> events_;
    bool eventStorageFailed_{};
    std::int64_t performanceCounterFrequency_{};
};

} // namespace PhantomCore
