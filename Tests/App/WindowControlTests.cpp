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
            Check(child != nullptr && IsWindowUnicode(child), "Native edit is Unicode.");
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
    std::cout << "Native Unicode input/IME lifecycle, window resize, display modes and renderer notification passed.\n";
    return 0;
}
