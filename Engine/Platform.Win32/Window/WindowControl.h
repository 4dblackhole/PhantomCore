#pragma once
#include "Input/TextInput.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>


namespace PhantomCore
{
struct WindowResolution
{
    std::uint32_t width{}, height{};
    bool operator==(const WindowResolution &) const = default;
};

// Where the client area (the game picture) sits: its top-left corner in
// pixels from the top-left corner of the monitor the window is on.
struct WindowPosition
{
    std::int32_t x{}, y{};
    bool operator==(const WindowPosition &) const = default;
};

// How the game window sits on its monitor. The client size is the game
// picture in both modes.
enum class WindowMode : std::uint8_t
{
    // Title bar and border. A client size that covers the whole monitor is
    // shown without them, so that size fills the screen.
    Windowed,
    // No title bar or border. The picture is centred on the monitor and a
    // black backdrop covers the rest of that monitor.
    Borderless,
};

// Run owns this service. Calls belong to the game-loop thread; resize
// notifications reach the renderer and Client on the next loop iteration.
class IWindowControl
{
  public:
    virtual ~IWindowControl() = default;
    [[nodiscard]] virtual WindowResolution ClientSize() const noexcept = 0;
    [[nodiscard]] virtual std::vector<WindowResolution> DisplayResolutions() const = 0;
    [[nodiscard]] virtual bool SetClientSize(WindowResolution size, std::string &error) = 0;
    // Optional for offscreen/test implementations, which stay Windowed.
    [[nodiscard]] virtual WindowMode Mode() const noexcept { return WindowMode::Windowed; }
    // Keeps the client size and places the window again for the new mode.
    [[nodiscard]] virtual bool SetMode(WindowMode mode, std::string &error)
    {
        error = "This window cannot change its mode.";
        return mode == Mode();
    }
    // Pixel size of the monitor the window is on; zero when there is none.
    [[nodiscard]] virtual WindowResolution MonitorSize() const noexcept { return {}; }
    // Where the client area is now.
    [[nodiscard]] virtual WindowPosition Position() const noexcept { return {}; }
    // Remembers a position and moves the window there; an empty value forgets
    // it and returns to the default placement of the mode. A position that
    // does not fit is brought to the nearest one that does: Borderless keeps
    // the whole picture on the monitor, Windowed keeps the framed window
    // inside the work area. The remembered position also applies to later
    // size and mode changes.
    [[nodiscard]] virtual bool SetPosition(std::optional<WindowPosition> position, std::string &error)
    {
        error = "This window cannot be moved.";
        return !position.has_value();
    }
    // True once after the user finished dragging the framed window; the
    // position is where it ended up and is remembered like SetPosition.
    [[nodiscard]] virtual bool TakeUserMove(WindowPosition &position) noexcept
    {
        static_cast<void>(position);
        return false;
    }
    // Optional for offscreen/test implementations. The caller owns the editor.
    [[nodiscard]] virtual std::unique_ptr<ITextInput> CreateTextInput() { return {}; }
};

} // namespace PhantomCore
