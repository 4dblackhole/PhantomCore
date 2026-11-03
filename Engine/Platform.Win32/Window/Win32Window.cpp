#include "Window/Win32Window.h"
#include "Input/Win32TextInput.h"

#include <algorithm>
#include <array>
#include <hidusage.h>
#include <stdexcept>
#include <string>


namespace
{
    constexpr wchar_t WindowClassName[] = L"PhantomCore.D3D12.Window";

    std::runtime_error MakeWin32Error(const char* operation)
    {
        return std::runtime_error(
            std::string(operation) + " failed with Win32 error " +
            std::to_string(GetLastError()));
    }

    std::uint16_t NormalizeVirtualKey(const RAWKEYBOARD& keyboard) noexcept
    {
        std::uint16_t key = keyboard.VKey;

        if (key == VK_SHIFT)
        {
            key = static_cast<std::uint16_t>(
                MapVirtualKeyW(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX));
        }
        else if (key == VK_CONTROL)
        {
            key = (keyboard.Flags & RI_KEY_E0) != 0 ? VK_RCONTROL : VK_LCONTROL;
        }
        else if (key == VK_MENU)
        {
            key = (keyboard.Flags & RI_KEY_E0) != 0 ? VK_RMENU : VK_LMENU;
        }

        return key;
    }
}

Win32Window::~Win32Window()
{
    ReleaseMouseCapture();

    if (window_ != nullptr)
    {
        DestroyWindow(window_);
        window_ = nullptr;
    }

    if (instance_ != nullptr)
    {
        UnregisterClassW(WindowClassName, instance_);
    }
}

void Win32Window::Initialize(const WindowConfig& config, InputState& input)
{
    instance_ = GetModuleHandleW(nullptr);
    input_ = &input;
    LARGE_INTEGER performanceCounterFrequency{};
    if (!QueryPerformanceFrequency(&performanceCounterFrequency))
    {
        throw MakeWin32Error("QueryPerformanceFrequency");
    }
    input_->SetPerformanceCounterFrequency(
        performanceCounterFrequency.QuadPart);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = StaticWindowProcedure;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = WindowClassName;

    if (RegisterClassExW(&windowClass) == 0)
    {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS)
        {
            throw MakeWin32Error("RegisterClassExW");
        }
    }

    RECT windowRectangle{
        0,
        0,
        static_cast<LONG>(config.clientWidth),
        static_cast<LONG>(config.clientHeight)};

    constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    if (!AdjustWindowRect(&windowRectangle, windowStyle, FALSE))
    {
        throw MakeWin32Error("AdjustWindowRect");
    }

    window_ = CreateWindowExW(
        0,
        WindowClassName,
        config.title.c_str(),
        windowStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRectangle.right - windowRectangle.left,
        windowRectangle.bottom - windowRectangle.top,
        nullptr,
        nullptr,
        instance_,
        this);

    if (window_ == nullptr)
    {
        throw MakeWin32Error("CreateWindowExW");
    }

    clientWidth_ = config.clientWidth;
    clientHeight_ = config.clientHeight;
    RegisterRawInputDevices();
    RefreshDisplayRate();

    ShowWindow(window_, config.visible ? SW_SHOW : SW_HIDE);
    UpdateWindow(window_);
}

bool Win32Window::PumpMessages()
{
    // Transient input data belongs to one engine Update.  Held state is
    // intentionally preserved by InputState::BeginUpdate.
    input_->BeginUpdate();

    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            return false;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (input_->eventStorageFailed_)
            throw std::runtime_error("Unable to retain the complete timestamped input stream.");
    }

    // UI placement needs an absolute client-space snapshot. Raw Input is
    // still used for button transitions, deltas, and QPC timestamps.
    UpdateMousePosition();

    return true;
}

void Win32Window::SetTitle(const std::wstring& title) const noexcept
{
    if (window_ != nullptr)
    {
        SetWindowTextW(window_, title.c_str());
    }
}

WindowResolution Win32Window::ClientSize() const noexcept
{
    return {clientWidth_, clientHeight_};
}

std::vector<WindowResolution> Win32Window::DisplayResolutions() const
{
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor))
        throw MakeWin32Error("GetMonitorInfoW");
    std::vector<WindowResolution> result;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsW(monitor.szDevice, index, &mode); ++index)
    {
        const WindowResolution size{mode.dmPelsWidth, mode.dmPelsHeight};
        if (size.width > 0 && size.height > 0 && size.width <= 16384 && size.height <= 16384 &&
            std::ranges::find(result, size) == result.end())
            result.push_back(size);
    }
    std::ranges::sort(result, [](auto a, auto b)
                      { return a.width < b.width || (a.width == b.width && a.height < b.height); });
    return result;
}

bool Win32Window::SetClientSize(WindowResolution size, std::string &error)
{
    error.clear();
    // Match the D3D12 texture dimension limit before converting to LONG.
    if (!window_ || size.width == 0 || size.height == 0 || size.width > 16384 || size.height > 16384)
    {
        error = "Window dimensions must be within 1..16384 pixels.";
        return false;
    }
    RECT outer{0, 0, static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&outer, style, GetMenu(window_) != nullptr, extended))
    {
        error = MakeWin32Error("AdjustWindowRectEx").what();
        return false;
    }
    if (IsZoomed(window_))
        ShowWindow(window_, SW_RESTORE);
    if (!SetWindowPos(window_, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
    {
        error = MakeWin32Error("SetWindowPos").what();
        return false;
    }
    return true;
}

HWND Win32Window::Handle() const noexcept
{
    return window_;
}

std::unique_ptr<ITextInput> Win32Window::CreateTextInput()
{
    return CreateWin32TextInput(window_);
}

std::uint32_t Win32Window::ClientWidth() const noexcept
{
    return clientWidth_;
}

std::uint32_t Win32Window::ClientHeight() const noexcept
{
    return clientHeight_;
}

bool Win32Window::IsMinimized() const noexcept
{
    return minimized_;
}

double Win32Window::RefreshRateHz() const noexcept
{
    return refreshRateHz_;
}

bool Win32Window::ConsumeResize(WindowSize& size) noexcept
{
    if (!resizePending_)
    {
        return false;
    }

    resizePending_ = false;
    size = {clientWidth_, clientHeight_};
    return true;
}

bool Win32Window::ConsumeRefreshRateChange(double& refreshRateHz) noexcept
{
    if (!refreshRateChanged_)
    {
        return false;
    }

    refreshRateChanged_ = false;
    refreshRateHz = refreshRateHz_;
    return true;
}

LRESULT CALLBACK Win32Window::StaticWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    Win32Window* self = nullptr;

    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<Win32Window*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<Win32Window*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self != nullptr)
    {
        return self->WindowProcedure(message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Win32Window::WindowProcedure(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && NotifyWin32TextInputChanged(reinterpret_cast<HWND>(lParam)))
            return 0;
        break;
    case WM_CTLCOLOREDIT:
        if (const auto brush = PaintWin32TextInput(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam)))
            return reinterpret_cast<LRESULT>(brush);
        break;
    case WM_INPUT:
        ProcessRawInput(reinterpret_cast<HRAWINPUT>(lParam));
        UpdateMouseCapture();
        return 0;

    case WM_ACTIVATEAPP:
        if (wParam == FALSE)
        {
            input_->Reset();
            ReleaseMouseCapture();
        }
        return 0;

    case WM_SIZE:
        minimized_ = wParam == SIZE_MINIMIZED;
        if (!minimized_)
        {
            clientWidth_ = static_cast<std::uint32_t>(LOWORD(lParam));
            clientHeight_ = static_cast<std::uint32_t>(HIWORD(lParam));
            resizePending_ = clientWidth_ > 0 && clientHeight_ > 0;
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_WINDOWPOSCHANGED:
        RefreshDisplayRate();
        break;

    case WM_SETCURSOR:
        if (mouseCaptured_ && LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;

    case WM_DESTROY:
        ReleaseMouseCapture();
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

void Win32Window::RegisterRawInputDevices()
{
    const std::array devices{
        RAWINPUTDEVICE{
            HID_USAGE_PAGE_GENERIC,
            HID_USAGE_GENERIC_KEYBOARD,
            0,
            window_},
        RAWINPUTDEVICE{
            HID_USAGE_PAGE_GENERIC,
            HID_USAGE_GENERIC_MOUSE,
            0,
            window_}};

    if (!::RegisterRawInputDevices(
            devices.data(),
            static_cast<UINT>(devices.size()),
            sizeof(RAWINPUTDEVICE)))
    {
        throw MakeWin32Error("RegisterRawInputDevices");
    }
}

void Win32Window::ProcessRawInput(const HRAWINPUT rawInputHandle)
{
    // Timestamp at message processing time with the same QPC source used
    // by Engine's clock.  Rhythm judgement should consume InputState's
    // ordered event stream rather than infer timing from rendered frames.
    LARGE_INTEGER timestamp{};
    QueryPerformanceCounter(&timestamp);

    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(
            rawInputHandle,
            RID_INPUT,
            &input,
            &size,
            sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
    {
        return;
    }

    if (input.header.dwType == RIM_TYPEKEYBOARD)
    {
        ProcessRawKeyboard(input.data.keyboard, timestamp.QuadPart);
    }
    else if (input.header.dwType == RIM_TYPEMOUSE)
    {
        ProcessRawMouse(input.data.mouse, timestamp.QuadPart);
    }
}

void Win32Window::ProcessRawKeyboard(
    const RAWKEYBOARD& keyboard,
    const std::int64_t timestamp)
{
    // Windows uses 0xFF for a fake key that should not enter application
    // state. Normalize modifiers before storing the Virtual-Key value.
    if (keyboard.VKey == 0xFF)
    {
        return;
    }

    const std::uint16_t key = NormalizeVirtualKey(keyboard);
    const bool isDown = (keyboard.Flags & RI_KEY_BREAK) == 0;
    input_->SetKey(key, isDown, timestamp);
}

void Win32Window::ProcessRawMouse(
    const RAWMOUSE& mouse,
    const std::int64_t timestamp)
{
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
    {
        input_->AddMouseDelta(mouse.lLastX, mouse.lLastY, timestamp);
    }

    const USHORT flags = mouse.usButtonFlags;
    const auto updateButton =
        [this, flags, timestamp](
            const USHORT downFlag,
            const USHORT upFlag,
            const MouseButton button)
    {
        if ((flags & downFlag) != 0)
        {
            input_->SetMouseButton(button, true, timestamp);
        }
        if ((flags & upFlag) != 0)
        {
            input_->SetMouseButton(button, false, timestamp);
        }
    };

    updateButton(
        RI_MOUSE_LEFT_BUTTON_DOWN,
        RI_MOUSE_LEFT_BUTTON_UP,
        MouseButton::Left);
    updateButton(
        RI_MOUSE_RIGHT_BUTTON_DOWN,
        RI_MOUSE_RIGHT_BUTTON_UP,
        MouseButton::Right);
    updateButton(
        RI_MOUSE_MIDDLE_BUTTON_DOWN,
        RI_MOUSE_MIDDLE_BUTTON_UP,
        MouseButton::Middle);
    updateButton(
        RI_MOUSE_BUTTON_4_DOWN,
        RI_MOUSE_BUTTON_4_UP,
        MouseButton::X1);
    updateButton(
        RI_MOUSE_BUTTON_5_DOWN,
        RI_MOUSE_BUTTON_5_UP,
        MouseButton::X2);

    if ((flags & RI_MOUSE_WHEEL) != 0)
    {
        const auto wheelDelta = static_cast<SHORT>(mouse.usButtonData);
        input_->AddMouseWheel(
            static_cast<float>(wheelDelta) /
                static_cast<float>(WHEEL_DELTA),
            timestamp);
    }
}

void Win32Window::UpdateMouseCapture()
{
    // Native editors retain their mouse selection/context-menu behavior.
    POINT cursor{};
    if (IsWin32TextInput(GetCapture()) ||
        (GetCursorPos(&cursor) && IsWin32TextInput(WindowFromPoint(cursor))))
    {
        ReleaseMouseCapture();
        return;
    }
    // Right-button look captures the cursor to the client rectangle; Raw
    // Input still supplies relative movement while the cursor is hidden.
    const bool shouldCapture =
        input_->IsMouseButtonDown(MouseButton::Right);

    if (shouldCapture == mouseCaptured_)
    {
        return;
    }

    if (!shouldCapture)
    {
        ReleaseMouseCapture();
        return;
    }

    SetCapture(window_);

    RECT clientRectangle{};
    if (GetClientRect(window_, &clientRectangle))
    {
        POINT upperLeft{clientRectangle.left, clientRectangle.top};
        POINT lowerRight{clientRectangle.right, clientRectangle.bottom};
        ClientToScreen(window_, &upperLeft);
        ClientToScreen(window_, &lowerRight);
        RECT screenRectangle{
            upperLeft.x,
            upperLeft.y,
            lowerRight.x,
            lowerRight.y};
        ClipCursor(&screenRectangle);
    }

    mouseCaptured_ = true;
}

void Win32Window::ReleaseMouseCapture()
{
    if (!mouseCaptured_)
    {
        return;
    }

    ClipCursor(nullptr);
    if (GetCapture() == window_)
    {
        ReleaseCapture();
    }
    mouseCaptured_ = false;
}

void Win32Window::UpdateMousePosition() noexcept
{
    if (window_ == nullptr || input_ == nullptr)
    {
        return;
    }

    POINT point{};
    RECT clientRectangle{};
    if (!GetCursorPos(&point) ||
        !ScreenToClient(window_, &point) ||
        !GetClientRect(window_, &clientRectangle))
    {
        input_->SetMousePosition(0, 0, false);
        return;
    }

    const bool inside =
        point.x >= clientRectangle.left && point.x < clientRectangle.right &&
        point.y >= clientRectangle.top && point.y < clientRectangle.bottom;
    input_->SetMousePosition(point.x, point.y, inside);
}

void Win32Window::RefreshDisplayRate() noexcept
{
    if (window_ == nullptr)
    {
        return;
    }

    const HMONITOR monitor = MonitorFromWindow(
        window_,
        MONITOR_DEFAULTTONEAREST);

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    DEVMODEW displayMode{};
    displayMode.dmSize = sizeof(displayMode);

    double newRate = 60.0;
    if (GetMonitorInfoW(monitor, &monitorInfo) &&
        EnumDisplaySettingsExW(
            monitorInfo.szDevice,
            ENUM_CURRENT_SETTINGS,
            &displayMode,
            0) &&
        displayMode.dmDisplayFrequency > 1)
    {
        newRate = static_cast<double>(displayMode.dmDisplayFrequency);
    }

    if (newRate != refreshRateHz_)
    {
        refreshRateHz_ = newRate;
        refreshRateChanged_ = true;
    }
}
