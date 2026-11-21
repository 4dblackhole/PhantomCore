#pragma once
#include "Input/TextInput.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>


namespace PhantomCore
{
struct WindowResolution
{
    std::uint32_t width{}, height{};
    bool operator==(const WindowResolution &) const = default;
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
    // Optional for offscreen/test implementations. The caller owns the editor.
    [[nodiscard]] virtual std::unique_ptr<ITextInput> CreateTextInput() { return {}; }
};

} // namespace PhantomCore
