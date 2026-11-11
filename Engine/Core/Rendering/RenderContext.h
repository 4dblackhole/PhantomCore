#pragma once

#include "Rendering/ImageReadback.h"

#include <cstdint>
#include <future>

namespace PhantomCore
{
class MeshRenderSystem;
class TextRenderSystem;
class Visual2DRenderSystem;
struct RenderFrameContext;

struct ClearColor
{
    float red{};
    float green{};
    float blue{};
    float alpha{1.0F};
};

// Non-owning view of the engine's current rendering frame.
struct RenderContext
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t frameIndex{};
    // Monotonic frame sequence, distinct from the reusable frame slot.
    std::uint64_t renderIndex{};
    MeshRenderSystem* meshRendering{};
    TextRenderSystem* textRendering{};
    Visual2DRenderSystem* visual2DRendering{};

    // Request the final RGBA image of this frame on the rendering thread.
    // Repeated requests share a result. Poll without blocking Update/Render.
    // Completion uses an existing frame-resource or shutdown wait, and the
    // completed CPU image remains usable after the engine is destroyed.
    [[nodiscard]] std::shared_future<RgbaImage> ReadImage() const;

private:
    friend struct RenderFrameContext;
    const RenderFrameContext* frame_{};
};

} // namespace PhantomCore
