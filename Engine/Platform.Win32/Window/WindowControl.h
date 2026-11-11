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

// Run owns this service. Calls belong to the game-loop thread; resize
// notifications reach the renderer and Client on the next loop iteration.
class IWindowControl
{
  public:
    virtual ~IWindowControl() = default;
    [[nodiscard]] virtual WindowResolution ClientSize() const noexcept = 0;
    [[nodiscard]] virtual std::vector<WindowResolution> DisplayResolutions() const = 0;
    [[nodiscard]] virtual bool SetClientSize(WindowResolution size, std::string &error) = 0;
    // Optional for offscreen/test implementations. The caller owns the editor.
    [[nodiscard]] virtual std::unique_ptr<ITextInput> CreateTextInput() { return {}; }
};

} // namespace PhantomCore
