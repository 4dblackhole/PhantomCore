#pragma once

// Texture feature: WIC decoding, uploads, and SRV descriptor sets.

#include "../../Math/Math.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <memory>
#include <span>
#include <vector>


// One descriptor table reserves this many entries.  Each entry can point
// at an independently sized Texture2D resource; this is not a
// D3D12 Texture2DArray and therefore does not require equal dimensions.
namespace PhantomCore
{
inline constexpr std::uint32_t MaxTexturesPerSet = 64;

struct TextureInfo final
{
    std::uint32_t width{};
    std::uint32_t height{};
};

struct UvTransform final
{
    Float2 scale{1.0F, 1.0F};
    Float2 offset{0.0F, 0.0F};
};

// Read-only texture-set contract exposed to materials and Client code.
// The concrete D3D12 allocation remains private to TextureManager.cpp.
class TextureSet
{
public:
    virtual ~TextureSet();

    TextureSet(const TextureSet&) = delete;
    TextureSet& operator=(const TextureSet&) = delete;

    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] const TextureInfo& Info(std::size_t index) const;

    // Returns a centered "cover" transform.  Sampling a rectangular
    // image on the requested aspect ratio crops the excess dimension,
    // filling the complete target without stretching or letterboxing.
    [[nodiscard]] UvTransform MakeCoverUvTransform(
        std::size_t index,
        float targetAspectRatio = 1.0F) const;

protected:
    TextureSet() = default;
    void AppendInfo(TextureInfo info);

private:
    std::vector<TextureInfo> textureInfo_;
};

using TextureSetHandle = std::shared_ptr<const TextureSet>;

// One GPU texture that can alternate between a render target and a
// shader resource. Its opaque implementation owns the texture, RTV and
// resource state; TextureManager owns the shared SRV descriptor heap.
class RenderTargetTexture
{
public:
    virtual ~RenderTargetTexture();

    RenderTargetTexture(const RenderTargetTexture&) = delete;
    RenderTargetTexture& operator=(const RenderTargetTexture&) = delete;

    [[nodiscard]] virtual const TextureSetHandle& Textures()
        const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t Width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t Height() const noexcept = 0;

protected:
    RenderTargetTexture() = default;
};

using RenderTargetTextureHandle =
    std::shared_ptr<RenderTargetTexture>;

// Decodes common image formats through WIC and owns the shader-visible
// descriptor heap used by textured materials. Upload lifetimes are retained
// until their fence completes; decoding can be prepared on a worker.
class TextureManager final
{
public:
    TextureManager();
    ~TextureManager();

    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    bool PrepareImages(std::span<const std::filesystem::path> paths, std::string& error);
    [[nodiscard]] TextureSetHandle LoadTextureSet(
        std::span<const std::filesystem::path> paths);
    // Append into the same descriptor table while retaining existing slots.
    [[nodiscard]] TextureSetHandle AppendTexture(
        const TextureSetHandle& textureSet, const std::filesystem::path& path);
    [[nodiscard]] RenderTargetTextureHandle CreateRenderTargetTexture(
        std::uint32_t width, std::uint32_t height);

private:
    friend struct TextureRenderingBackend;
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace PhantomCore
