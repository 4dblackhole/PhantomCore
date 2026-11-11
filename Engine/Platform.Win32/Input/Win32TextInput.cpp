#include "Input/Win32TextInput.h"
#include <commctrl.h>
#include <imm.h>
#include <limits>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")

namespace PhantomCore
{
namespace
{
    constexpr wchar_t InputProperty[] = L"PhantomCore.TextInput";
    COLORREF Rgb(std::uint32_t color) noexcept
    {
        return RGB((color >> 16) & 255, (color >> 8) & 255, color & 255);
    }

    class Win32TextInput final : public ITextInput
    {
      public:
        ~Win32TextInput() override
        {
            if (window_) DestroyWindow(window_);
            if (font_) DeleteObject(font_);
            if (brush_) DeleteObject(brush_);
        }
        void Initialize(HWND parent)
        {
            parent_ = parent;
            window_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_LEFT | ES_AUTOHSCROLL,
                                      0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window_ || !SetPropW(window_, InputProperty, this) ||
                !SetWindowSubclass(window_, Procedure, 1, reinterpret_cast<DWORD_PTR>(this)))
                throw std::runtime_error("Failed to create a Unicode text input control.");
            SetColors(0x243F5B, 0xFFFFFF);
            SetFont(L"Segoe UI", 16);
        }
        void SetText(std::wstring_view text) override
        {
            if (text.find(L'\0') != std::wstring_view::npos)
                throw std::invalid_argument("Text input cannot contain embedded nulls.");
            if (ReadText() != text && !SetWindowTextW(window_, std::wstring{text}.c_str()))
                throw std::runtime_error("Failed to update text input content.");
        }
        void SetPlaceholder(std::wstring_view text) override
        {
            if (placeholder_ == text) return;
            if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                throw std::length_error("Text input placeholder is too long.");
            placeholder_ = text;
            nativeCue_ = SendMessageW(window_, EM_SETCUEBANNER, FALSE,
                                     reinterpret_cast<LPARAM>(placeholder_.c_str())) != 0;
            InvalidateRect(window_, nullptr, TRUE);
        }
        void SetBounds(TextInputBounds bounds) override
        {
            constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
            if (bounds.width > maximum || bounds.height > maximum)
                throw std::out_of_range("Text input bounds exceed native window coordinates.");
            if (!SetWindowPos(window_, nullptr, bounds.x, bounds.y, static_cast<int>(bounds.width),
                              static_cast<int>(bounds.height), SWP_NOACTIVATE | SWP_NOZORDER))
                throw std::runtime_error("Failed to position text input.");
        }
        void SetFont(std::wstring_view family, std::uint32_t pixelHeight) override
        {
            if (pixelHeight == 0 || pixelHeight > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
                throw std::out_of_range("Text input font height must be positive.");
            if (family == fontFamily_ && pixelHeight == fontHeight_) return;
            const std::wstring name{family};
            const auto font = CreateFontW(-static_cast<int>(pixelHeight), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, name.c_str());
            if (!font) throw std::runtime_error("Failed to create text input font.");
            SendMessageW(window_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (font_) DeleteObject(font_);
            font_ = font;
            fontFamily_ = name;
            fontHeight_ = pixelHeight;
        }
        void SetColors(std::uint32_t foreground, std::uint32_t background) override
        {
            if (brush_ && foreground == foreground_ && background == background_) return;
            const auto brush = CreateSolidBrush(Rgb(background));
            if (!brush) throw std::runtime_error("Failed to create text input background.");
            if (brush_) DeleteObject(brush_);
            brush_ = brush;
            foreground_ = foreground;
            background_ = background;
            InvalidateRect(window_, nullptr, TRUE);
        }
        void SetVisible(bool visible) noexcept override
        {
            if (!window_ || visible == visible_) return;
            visible_ = visible;
            if (!visible)
            {
                if (HasFocus()) SetFocus(parent_);
                submit_ = cancel_ = false;
            }
            ShowWindow(window_, visible ? SW_SHOWNA : SW_HIDE);
        }
        void Focus(bool selectAll) noexcept override
        {
            if (!window_) return;
            SetFocus(window_);
            if (selectAll) SendMessageW(window_, EM_SETSEL, 0, -1);
        }
        bool HasFocus() const noexcept override { return window_ && GetFocus() == window_; }
        void Blur() noexcept override { if (HasFocus()) SetFocus(parent_); }
        bool TakeText(std::wstring &text) override
        {
            if (!changed_ || IsComposing()) return false;
            auto value = ReadText();
            changed_ = false;
            text = std::move(value);
            return true;
        }
        bool TakeSubmit() noexcept override { return std::exchange(submit_, false); }
        bool TakeCancel() noexcept override { return std::exchange(cancel_, false); }
        void Changed() noexcept { changed_ = true; }
        HBRUSH Paint(HDC dc) const noexcept
        {
            SetTextColor(dc, Rgb(foreground_));
            SetBkColor(dc, Rgb(background_));
            return brush_;
        }

      private:
        void PaintPlaceholder(HWND window, HDC target = nullptr) const noexcept
        {
            // Cue banners require a v6 common-controls manifest. Keep the same
            // empty/unfocused hint for consumers that use the default EDIT class.
            if (nativeCue_ || placeholder_.empty() || HasFocus() || GetWindowTextLengthW(window) != 0) return;
            HDC dc = target ? target : GetDC(window);
            if (!dc) return;
            const int saved = SaveDC(dc);
            if (saved != 0)
            {
                RECT bounds{};
                GetClientRect(window, &bounds);
                bounds.left += 1;
                SelectObject(dc, font_);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, Rgb(foreground_));
                DrawTextW(dc, placeholder_.c_str(), static_cast<int>(placeholder_.size()), &bounds,
                          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                RestoreDC(dc, saved);
            }
            if (!target) ReleaseDC(window, dc);
        }
        std::wstring ReadText() const
        {
            if (!window_) return {};
            const int length = GetWindowTextLengthW(window_);
            std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
            const int copied = GetWindowTextW(window_, value.data(), length + 1);
            value.resize(static_cast<std::size_t>(copied));
            return value;
        }
        bool IsComposing() const noexcept
        {
            if (composing_) return true;
            const auto context = window_ ? ImmGetContext(window_) : nullptr;
            if (!context) return false;
            const bool active = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0;
            ImmReleaseContext(window_, context);
            return active;
        }
        static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR, DWORD_PTR data) noexcept
        {
            auto &self = *reinterpret_cast<Win32TextInput *>(data);
            if (message == WM_NCDESTROY)
            {
                RemovePropW(window, InputProperty);
                RemoveWindowSubclass(window, Procedure, 1);
                self.window_ = nullptr;
            }
            else if (message == WM_IME_STARTCOMPOSITION)
                self.composing_ = true;
            else if (message == WM_IME_ENDCOMPOSITION || message == WM_KILLFOCUS)
                self.composing_ = false;
            else if (message == WM_KEYDOWN && !self.IsComposing())
            {
                if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
                {
                    SendMessageW(window, EM_SETSEL, 0, -1);
                    return 0;
                }
                if (wParam == VK_RETURN) { self.submit_ = true; return 0; }
                if (wParam == VK_ESCAPE) { self.cancel_ = true; return 0; }
            }
            // Enter/Escape commands above must not produce a native beep.
            if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == 1)) return 0;
            const auto result = DefSubclassProc(window, message, wParam, lParam);
            if (message == WM_PAINT) self.PaintPlaceholder(window);
            else if (message == WM_PRINTCLIENT) self.PaintPlaceholder(window, reinterpret_cast<HDC>(wParam));
            else if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
                InvalidateRect(window, nullptr, TRUE);
            return result;
        }
        HWND parent_{}, window_{};
        HFONT font_{};
        HBRUSH brush_{};
        std::wstring fontFamily_;
        std::wstring placeholder_;
        std::uint32_t fontHeight_{}, foreground_{}, background_{};
        bool changed_{}, composing_{}, submit_{}, cancel_{}, visible_{}, nativeCue_{};
    };
    Win32TextInput *Find(HWND window) noexcept
    {
        return window ? static_cast<Win32TextInput *>(GetPropW(window, InputProperty)) : nullptr;
    }
}

std::unique_ptr<ITextInput> CreateWin32TextInput(HWND parent)
{
    auto input = std::make_unique<Win32TextInput>();
    input->Initialize(parent);
    return input;
}
bool IsWin32TextInput(HWND window) noexcept { return Find(window) != nullptr; }
bool NotifyWin32TextInputChanged(HWND window) noexcept
{
    if (auto *input = Find(window)) { input->Changed(); return true; }
    return false;
}
HBRUSH PaintWin32TextInput(HWND window, HDC dc) noexcept
{
    const auto *input = Find(window);
    return input ? input->Paint(dc) : nullptr;
}

} // namespace PhantomCore
