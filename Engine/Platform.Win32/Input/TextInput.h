#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace PhantomCore
{
struct TextInputBounds
{
    std::int32_t x{}, y{};
    std::uint32_t width{}, height{};
};

// A single-line Unicode editor owned by the caller; the creating window must
// outlive it. Calls belong to the window thread. The OS owns IME, selection,
// caret movement, deletion and clipboard editing, independently of Raw Input.
class ITextInput
{
  public:
    virtual ~ITextInput() = default;
    virtual void SetText(std::wstring_view text) = 0;
    virtual void SetPlaceholder(std::wstring_view text) = 0;
    virtual void SetBounds(TextInputBounds bounds) = 0;
    virtual void SetFont(std::wstring_view family, std::uint32_t pixelHeight) = 0;
    // Colors use 0xRRGGBB.
    virtual void SetColors(std::uint32_t foreground, std::uint32_t background) = 0;
    virtual void SetVisible(bool visible) noexcept = 0;
    virtual void Focus(bool selectAll = false) noexcept = 0;
    virtual void Blur() noexcept = 0;
    [[nodiscard]] virtual bool HasFocus() const noexcept = 0;
    // Reports committed text only; composition remains visible in the editor.
    virtual bool TakeText(std::wstring &text) = 0;
    virtual bool TakeSubmit() noexcept = 0;
    virtual bool TakeCancel() noexcept = 0;
};

} // namespace PhantomCore
