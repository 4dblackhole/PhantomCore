#pragma once

#include "Input/Input.h"
#include "Window/WindowControl.h"

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>


namespace PhantomCore
{
struct WindowConfig
{
    std::wstring title{L"My Rhythm Game"};
    std::uint32_t clientWidth{1280};
    std::uint32_t clientHeight{720};
    bool visible{true};
    WindowMode mode{WindowMode::Windowed};
    std::optional<WindowPosition> position{};
};

struct WindowSize
{
    std::uint32_t width{};
    std::uint32_t height{};
};

// Owns the native game window and translates messages into engine state.
class Win32Window final : public IWindowControl
{
public:
    Win32Window() = default;
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    void Initialize(const WindowConfig& config, InputState& input);
    [[nodiscard]] bool PumpMessages();
    void SetTitle(const std::wstring& title) const noexcept;

    [[nodiscard]] WindowResolution ClientSize() const noexcept override;
    [[nodiscard]] std::vector<WindowResolution> DisplayResolutions() const override;
    [[nodiscard]] bool SetClientSize(WindowResolution size, std::string &error) override;
    [[nodiscard]] WindowMode Mode() const noexcept override;
    [[nodiscard]] bool SetMode(WindowMode mode, std::string &error) override;
    [[nodiscard]] WindowResolution MonitorSize() const noexcept override;
    [[nodiscard]] WindowPosition Position() const noexcept override;
    [[nodiscard]] bool SetPosition(std::optional<WindowPosition> position, std::string &error) override;
    [[nodiscard]] bool TakeUserMove(WindowPosition &position) noexcept override;
    [[nodiscard]] std::unique_ptr<ITextInput> CreateTextInput() override;

    [[nodiscard]] HWND Handle() const noexcept;
    [[nodiscard]] std::uint32_t ClientWidth() const noexcept;
    [[nodiscard]] std::uint32_t ClientHeight() const noexcept;
    [[nodiscard]] bool IsMinimized() const noexcept;
    // Shown, and another program's window is in front. A window of this
    // process in front (a dialog) keeps the game in the foreground.
    [[nodiscard]] bool IsInBackground() const noexcept;
    [[nodiscard]] double RefreshRateHz() const noexcept;

    [[nodiscard]] bool ConsumeResize(WindowSize& size) noexcept;
    [[nodiscard]] bool ConsumeRefreshRateChange(double& refreshRateHz) noexcept;

private:
    static LRESULT CALLBACK StaticWindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    LRESULT WindowProcedure(UINT message, WPARAM wParam, LPARAM lParam);
    void RegisterRawInputDevices();
    void ProcessRawInput(HRAWINPUT rawInputHandle);
    void ProcessRawKeyboard(
        const RAWKEYBOARD& keyboard,
        std::int64_t timestamp);
    void ProcessRawMouse(
        const RAWMOUSE& mouse,
        std::int64_t timestamp);
    void UpdateMouseCapture();
    void UpdateMousePosition() noexcept;
    void ReleaseMouseCapture();
    void RefreshDisplayRate() noexcept;
    [[nodiscard]] bool Place(WindowResolution size, WindowMode mode, std::string &error);
    void UpdateBackdrop() noexcept;
    void UpdateCompositionPin(bool coversMonitor) noexcept;

    HINSTANCE instance_{};
    HWND window_{};
    // Black window behind a Borderless picture that is smaller than its monitor.
    HWND backdrop_{};
    // One-pixel child that keeps a monitor-sized window composed (no tearing).
    HWND compositionPin_{};
    WindowMode mode_{WindowMode::Windowed};
    // Client-area position the Client or a finished drag asked for; empty
    // uses the default placement of the mode.
    std::optional<WindowPosition> placement_{};
    bool userMoved_{};
    InputState* input_{};
    std::uint32_t clientWidth_{};
    std::uint32_t clientHeight_{};
    bool resizePending_{};
    bool minimized_{};
    bool visible_{};
    bool processInFront_{};
    bool mouseCaptured_{};
    bool refreshRateChanged_{};
    double refreshRateHz_{60.0};
    // Monitor and whole-number rate the exact rate was last read for.
    HMONITOR rateMonitor_{};
    DWORD nominalRateHz_{};
};

} // namespace PhantomCore
