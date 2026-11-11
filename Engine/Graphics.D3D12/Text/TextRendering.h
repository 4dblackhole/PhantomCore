#pragma once

// DirectWrite-backed text layout with a native D3D12 glyph-atlas renderer.
// Public types intentionally hide DirectWrite objects and atlas resources.

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>


enum class TextHorizontalAlignment : std::uint8_t
{
    Leading,
    Center,
    Trailing,
};

enum class TextVerticalAlignment : std::uint8_t
{
    Near,
    Center,
    Far,
};

class Font final
{
public:
    ~Font();

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    [[nodiscard]] std::wstring_view FamilyName() const noexcept;

private:
    friend class TextRenderSystem;
    struct Impl;

    explicit Font(std::shared_ptr<Impl> implementation);
    std::shared_ptr<Impl> implementation_;
};

using FontHandle = std::shared_ptr<const Font>;

// Immutable shaping/placement result. Atlas locations are resolved only
// when visible glyphs are submitted, so eviction never invalidates it.
class TextLayout final
{
  public:
    ~TextLayout();
    [[nodiscard]] DirectX::XMFLOAT2 SizePixels() const noexcept;

  private:
    friend class TextRenderSystem;
    struct Impl;
    explicit TextLayout(std::shared_ptr<Impl> implementation);
    std::shared_ptr<Impl> implementation_;
};
using TextLayoutHandle = std::shared_ptr<const TextLayout>;

struct TextRenderStatistics
{
    std::size_t layoutCreations{};
    std::size_t glyphRasterizations{};
    std::size_t submittedGlyphs{};
    std::size_t uploadAllocations{};
    std::size_t uploadBytes{};
    std::size_t atlasPages{};
    std::size_t atlasEvictions{};
};

struct TextStyle
{
    FontHandle font;
    float fontSizePixels{20.0F};
    DirectX::XMFLOAT4 color{1.0F, 1.0F, 1.0F, 1.0F};
    // OpenType/DirectWrite weight, 1..999; 400 is normal, 700 is bold.
    std::uint16_t fontWeight{400};
};

struct TextDrawCommand
{
    DirectX::XMFLOAT2 positionPixels{};
    DirectX::XMFLOAT2 layoutSizePixels{512.0F, 128.0F};
    // Smaller values are closer to the viewer. Screen UI supplies this
    // from its sorted draw-command order so popup text obeys the same
    // Z-order as its rectangle and image background.
    float depth{};
    // Maps glyph pixel coordinates to final viewport pixel coordinates.
    // Visual2D supplies its hierarchical XYZ transform; ordinary text
    // submissions use the identity default.
    DirectX::XMFLOAT4X4 transform{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
    TextHorizontalAlignment horizontalAlignment{
        TextHorizontalAlignment::Leading};
    TextVerticalAlignment verticalAlignment{
        TextVerticalAlignment::Near};
    // Pixel-space {left, top, right, bottom}. The default leaves text
    // unclipped inside the current render target.
    DirectX::XMFLOAT4 clipRectPixels{
        -1.0e9F, -1.0e9F, 1.0e9F, 1.0e9F};
    TextStyle style{};
};

// Text is submitted while a RenderContext is open. Prepared layouts retain
// shaping; Submit records visible glyph instances. The engine uploads cache
// misses and records the text pass at the end of the frame.
class TextRenderSystem final
{
public:
    TextRenderSystem();
    ~TextRenderSystem();

    TextRenderSystem(const TextRenderSystem&) = delete;
    TextRenderSystem& operator=(const TextRenderSystem&) = delete;

    [[nodiscard]] FontHandle LoadSystemFont(
        std::wstring_view familyName);
    [[nodiscard]] FontHandle LoadFontFile(
        const std::filesystem::path& fontFile);

    [[nodiscard]] TextLayoutHandle CreateLayout(std::wstring_view text, const TextDrawCommand &command);
    // Uses the prepared font/size/alignment; command contributes only
    // origin, transform, color, depth and clip. Handles may outlive LRU entries.
    void Submit(const TextLayoutHandle &layout, const TextDrawCommand &command);
    [[nodiscard]] TextRenderStatistics Statistics() const noexcept;

    void Submit(
        std::wstring_view text,
        const TextDrawCommand& command);

private:
    friend struct TextRenderingBackend;
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};
