#pragma once

#include "Input/Input.h"
#include "Window/WindowControl.h"

#include <Windows.h>

#include <cstdint>
#include <string>


namespace PhantomCore
{
struct WindowConfig
{
    std::wstring title{L"My Rhythm Game"};
    std::uint32_t clientWidth{1280};
    std::uint32_t clientHeight{720};
    bool visible{true};
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
    [[nodiscard]] std::unique_ptr<ITextInput> CreateTextInput() override;

    [[nodiscard]] HWND Handle() const noexcept;
    [[nodiscard]] std::uint32_t ClientWidth() const noexcept;
    [[nodiscard]] std::uint32_t ClientHeight() const noexcept;
    [[nodiscard]] bool IsMinimized() const noexcept;
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

    HINSTANCE instance_{};
    HWND window_{};
    InputState* input_{};
    std::uint32_t clientWidth_{};
    std::uint32_t clientHeight_{};
    bool resizePending_{};
    bool minimized_{};
    bool mouseCaptured_{};
    bool refreshRateChanged_{};
    double refreshRateHz_{60.0};
};

} // namespace PhantomCore
