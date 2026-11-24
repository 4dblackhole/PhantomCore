#include "WindowControlTests.h"
#include "PhantomCore.h"
#include <iostream>
#include <Windows.h>
#include <algorithm>
#include <stdexcept>

using namespace PhantomCore;

namespace
{
    void Check(bool value, const char *message)
    {
        if (!value)
            throw std::runtime_error(message);
    }
    struct ResizeReport
    {
        bool resized{}, rendered{}, shutdown{};
    };
    // Counts the position requests the native edit receives while it is installed.
    LONG_PTR previousEditProcedure{};
    int editPositionRequests{};
    LRESULT CALLBACK CountEditPositionRequests(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_WINDOWPOSCHANGING)
            ++editPositionRequests;
        return CallWindowProcW(reinterpret_cast<WNDPROC>(previousEditProcedure), window, message, wParam, lParam);
    }
    // Views place their editors on every Update; only a changed position may reach the window.
    void TestUnchangedBounds(HWND child, ITextInput &editor)
    {
        previousEditProcedure =
            SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CountEditPositionRequests));
        editPositionRequests = 0;
        for (int repeat = 0; repeat < 100; ++repeat)
            editor.SetBounds({24, 32, 320, 28});
        const int unchanged = editPositionRequests;
        editor.SetBounds({26, 32, 320, 28});
        const int moved = editPositionRequests;
        editor.SetBounds({24, 32, 320, 28});
        const int restored = editPositionRequests;
        SetWindowLongPtrW(child, GWLP_WNDPROC, previousEditProcedure);
        Check(unchanged == 0, "Unchanged editor bounds do not reach the native window.");
        Check(moved == 1 && restored == 2, "Each changed editor position moves the native window once.");
    }
    void TestPlaceholder(HWND child, ITextInput &editor)
    {
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) throw std::runtime_error("Create placeholder capture DC.");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 320;
        info.bmiHeader.biHeight = -28;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *data{};
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!bitmap || !data)
        {
            if (bitmap) DeleteObject(bitmap);
            DeleteDC(dc);
            throw std::runtime_error("Create placeholder capture bitmap.");
        }
        auto *pixels = static_cast<std::uint32_t *>(data);
        const auto previous = SelectObject(dc, bitmap);
        const auto painted = [&]
        {
            std::fill_n(pixels, 320 * 28, 0xFFFFFFFFU);
            SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
            GdiFlush();
            return std::count_if(pixels, pixels + 320 * 28,
                                 [](auto pixel) { return (pixel & 0xFFFFFFU) != 0xFFFFFFU; });
        };
        editor.SetPlaceholder(L"");
        const auto empty = painted();
        editor.SetPlaceholder(L"Search songs");
        const auto hinted = painted();
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        Check(hinted > empty + 10, "Placeholder paints without requiring an application theme manifest.");
    }
    class ResizeClient final : public IGameClient
    {
      public:
        explicit ResizeClient(ResizeReport &report) : report_(report)
        {
        }
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices &services) override
        {
            window_ = services.windowControl;
            Check(window_ != nullptr, "Run provides window control.");
            TestTextInput();
            const auto initial = window_->ClientSize();
            std::string error;
            Check(!window_->SetClientSize({0, 720}, error) && !error.empty() && window_->ClientSize() == initial,
                  "Invalid resize preserves dimensions.");
            Check(!window_->SetClientSize({16385, 720}, error), "Reject dimensions exceeding the graphics limit.");
            const auto modes = window_->DisplayResolutions();
            Check(!modes.empty(), "Enumerate native display modes.");
            for (std::size_t i = 0; i < modes.size(); ++i)
                for (std::size_t j = i + 1; j < modes.size(); ++j)
                    Check(modes[i] != modes[j], "Deduplicate refresh rates.");
            Check(window_->SetClientSize({960, 540}, error) && error.empty(), "Native resize accepted.");
            TestWindowModes();
            TestWindowPosition();
            Check(!report_.resized, "OnResize is deferred outside initialization.");
        }
        bool Update(const UpdateContext &context) override
        {
            Check(context.totalSeconds < 10, "Resize completes promptly.");
            return !report_.rendered;
        }
        void Render(const RenderContext &context) override
        {
            Check(report_.resized && window_->ClientSize() == WindowResolution{960, 540},
                  "Renderer receives native size before rendering.");
            Check(context.width == 960 && context.height == 540, "Swap chain matches client size.");
            report_.rendered = true;
        }
        void OnResize(std::uint32_t width, std::uint32_t height) override
        {
            report_.resized = width == 960 && height == 540;
        }
        void Shutdown() noexcept override
        {
            report_.shutdown = true;
            window_ = nullptr;
        }

      private:
        void TestTextInput()
        {
            auto editor = window_->CreateTextInput();
            Check(editor != nullptr, "Window creates Unicode editor.");
            editor->SetBounds({24, 32, 320, 28});
            editor->SetFont(L"Segoe UI", 18);
            editor->SetColors(0x243F5B, 0xFFFFFF);
            editor->SetPlaceholder(L"Search songs");
            editor->SetVisible(true);
            // Locate only the control just created in this test's own window.
            HWND parent = FindWindowW(L"PhantomCore.D3D12.Window", nullptr);
            DWORD process{};
            GetWindowThreadProcessId(parent, &process);
            Check(process == GetCurrentProcessId(), "Test owns native window.");
            HWND child = FindWindowExW(parent, nullptr, L"EDIT", nullptr);
            if (child == nullptr)
            {
                throw std::runtime_error("Native edit control was not created.");
            }
            Check(IsWindowUnicode(child), "Native edit is Unicode.");
            TestUnchangedBounds(child, *editor);
            TestPlaceholder(child, *editor);
            editor->SetText(L"Angel Dream");
            std::wstring text;
            Check(editor->TakeText(text) && text == L"Angel Dream", "Committed text notification.");
            Check(!editor->TakeText(text), "Text notifications are consumed once.");
            SendMessageW(child, EM_SETSEL, 0, -1);
            const wchar_t *foreign = L"천사의 꿈 天使の夢 café \U0001F3B5";
            SendMessageW(child, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(foreign));
            Check(editor->TakeText(text) && text == foreign, "Unicode insertion preserves foreign text and surrogate pairs.");
            SendMessageW(child, WM_IME_STARTCOMPOSITION, 0, 0);
            editor->SetText(L"조합");
            Check(!editor->TakeText(text), "Composition is not published as a committed query.");
            SendMessageW(child, WM_KEYDOWN, VK_RETURN, 0);
            SendMessageW(child, WM_KEYDOWN, VK_ESCAPE, 0);
            Check(!editor->TakeSubmit() && !editor->TakeCancel(), "IME commands do not submit/cancel editor.");
            SendMessageW(child, WM_IME_ENDCOMPOSITION, 0, 0);
            Check(editor->TakeText(text) && text == L"조합", "Composition publishes when committed.");
            SendMessageW(child, WM_KEYDOWN, VK_RETURN, 0);
            Check(editor->TakeSubmit() && !editor->TakeSubmit(), "Enter submit is consumed once.");
            SendMessageW(child, WM_KEYDOWN, VK_ESCAPE, 0);
            Check(editor->TakeCancel() && !editor->TakeCancel(), "Escape cancel is consumed once.");
            editor->SetVisible(false);
            Check((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) == 0, "Hide native editor.");
            editor.reset();
            Check(!IsWindow(child), "Editor owns and releases native control.");
        }
        // Runs on the hidden 960x540 window and leaves it Windowed at that size.
        void TestWindowModes()
        {
            HWND native = FindWindowW(L"PhantomCore.D3D12.Window", nullptr);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            Check(GetMonitorInfoW(MonitorFromWindow(native, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
                  "Read the test monitor.");
            const RECT screen = monitor.rcMonitor;
            const LONG screenWidth = screen.right - screen.left;
            const LONG screenHeight = screen.bottom - screen.top;
            Check(screenWidth > 960 && screenHeight > 540, "The window mode test needs a monitor larger than 960x540.");
            const auto framed = [native] { return (GetWindowLongPtrW(native, GWL_STYLE) & WS_CAPTION) == WS_CAPTION; };
            const auto outer = [native]
            {
                RECT rectangle{};
                GetWindowRect(native, &rectangle);
                return rectangle;
            };
            std::string error;
            Check(window_->Mode() == WindowMode::Windowed && framed(), "A window starts framed.");

            Check(window_->SetMode(WindowMode::Borderless, error) && window_->Mode() == WindowMode::Borderless,
                  "Borderless mode accepted.");
            Check(!framed() && window_->ClientSize() == WindowResolution{960, 540},
                  "Borderless drops the frame and keeps the client size.");
            Check(outer().left - screen.left == (screenWidth - 960) / 2 &&
                      outer().top - screen.top == (screenHeight - 540) / 2,
                  "Borderless centres the picture on its monitor.");
            Check(FindWindowW(L"PhantomCore.D3D12.Backdrop", nullptr) == nullptr,
                  "A hidden window shows no backdrop.");

            Check(window_->SetMode(WindowMode::Windowed, error) && framed(), "Windowed mode restores the frame.");
            Check(window_->ClientSize() == WindowResolution{960, 540}, "Changing the mode keeps the client size.");
            const WindowResolution full{static_cast<std::uint32_t>(screenWidth),
                                        static_cast<std::uint32_t>(screenHeight)};
            Check(window_->SetClientSize(full, error) && !framed(),
                  "A windowed size that covers the monitor has no frame.");
            Check(outer().left == screen.left && outer().top == screen.top && window_->ClientSize() == full,
                  "A monitor-sized picture fills the monitor.");
            // A window alone on its monitor would be shown without the compositor and could tear;
            // the one-pixel child in its corner keeps it composed.
            const auto pinShown = [native]
            {
                const HWND pin = FindWindowExW(native, nullptr, L"PhantomCore.D3D12.CompositionPin", nullptr);
                return pin != nullptr && (GetWindowLongPtrW(pin, GWL_STYLE) & WS_VISIBLE) != 0;
            };
            const HWND pin = FindWindowExW(native, nullptr, L"PhantomCore.D3D12.CompositionPin", nullptr);
            RECT pinRectangle{};
            Check(pinShown() && GetWindowRect(pin, &pinRectangle) != FALSE,
                  "A monitor-sized window shows its composition pin.");
            const bool onePixel = pinRectangle.right - pinRectangle.left == 1 && pinRectangle.bottom - pinRectangle.top == 1;
            Check(onePixel && pinRectangle.right == screen.right && pinRectangle.bottom == screen.bottom,
                  "The composition pin is the bottom-right pixel.");
            Check(window_->SetClientSize({960, 540}, error) && framed(),
                  "A smaller windowed size gets its frame back.");
            Check(!pinShown(), "A window smaller than its monitor hides the composition pin.");
            const RECT restored = outer();
            const bool onScreen = restored.left >= monitor.rcWork.left && restored.top >= monitor.rcWork.top;
            Check(onScreen && window_->ClientSize() == WindowResolution{960, 540},
                  "The framed window returns inside the work area.");
        }
        // Runs on the hidden, framed 960x540 window and leaves it that way with no remembered position.
        void TestWindowPosition()
        {
            HWND native = FindWindowW(L"PhantomCore.D3D12.Window", nullptr);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            Check(GetMonitorInfoW(MonitorFromWindow(native, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
                  "Read the test monitor.");
            const RECT screen = monitor.rcMonitor;
            const RECT work = monitor.rcWork;
            const auto screenWidth = static_cast<std::int32_t>(screen.right - screen.left);
            const auto screenHeight = static_cast<std::int32_t>(screen.bottom - screen.top);
            const auto outer = [native]
            {
                RECT rectangle{};
                GetWindowRect(native, &rectangle);
                return rectangle;
            };
            std::string error;
            Check(window_->MonitorSize() == WindowResolution{static_cast<std::uint32_t>(screenWidth),
                                                             static_cast<std::uint32_t>(screenHeight)},
                  "The monitor size is the size of the window's monitor.");

            Check(window_->SetMode(WindowMode::Borderless, error), "Borderless mode accepted.");
            Check(window_->SetPosition(WindowPosition{100, 50}, error) && window_->Position() == WindowPosition{100, 50},
                  "Borderless places the picture at the requested position.");
            Check(outer().left == screen.left + 100 && outer().top == screen.top + 50,
                  "The position is measured from the monitor's top-left corner.");
            Check(window_->SetPosition(WindowPosition{-500, 100000}, error) &&
                      window_->Position() == WindowPosition{0, screenHeight - 540},
                  "A position outside the monitor is brought back inside it.");
            Check(window_->SetPosition(std::nullopt, error) &&
                      window_->Position() == WindowPosition{(screenWidth - 960) / 2, (screenHeight - 540) / 2},
                  "Forgetting the position centres the picture again.");

            Check(window_->SetPosition(WindowPosition{100, 50}, error) && window_->SetMode(WindowMode::Windowed, error),
                  "Windowed mode accepted with a remembered position.");
            const RECT framedOuter = outer();
            const bool insideWork = framedOuter.left >= work.left && framedOuter.top >= work.top;
            Check(insideWork && framedOuter.right <= work.right && framedOuter.bottom <= work.bottom,
                  "A framed window with a remembered position stays inside the work area.");
            Check(window_->SetPosition(WindowPosition{-10000, -10000}, error) && outer().left >= work.left &&
                      outer().top >= work.top,
                  "A framed window is not moved above or left of the work area.");

            WindowPosition moved{};
            Check(!window_->TakeUserMove(moved), "No user move is reported before one happens.");
            SetWindowPos(native, nullptr, work.left + 40, work.top + 30, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(native, WM_EXITSIZEMOVE, 0, 0);
            Check(window_->TakeUserMove(moved) && moved == window_->Position(),
                  "A finished drag reports where the window ended up.");
            Check(!window_->TakeUserMove(moved), "A user move is reported once.");
            Check(window_->SetClientSize({800, 450}, error) && window_->Position() == moved,
                  "A size change keeps the position the user dragged to.");
            Check(window_->SetClientSize({960, 540}, error) && window_->SetPosition(std::nullopt, error),
                  "The window returns to 960x540 with no remembered position.");
        }
        ResizeReport &report_;
        IWindowControl *window_{};
    };
} // namespace
int TestWindowControl()
{
    ResizeReport report;
    const auto result = Run(std::make_unique<ResizeClient>(report));
    if (result != 0 || !report.resized || !report.rendered || !report.shutdown)
        return 1;
    std::cout << "Native Unicode input/IME lifecycle, window resize, window modes and position, display modes and renderer notification passed.\n";
    return 0;
}
