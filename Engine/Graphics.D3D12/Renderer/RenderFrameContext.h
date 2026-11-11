#pragma once

#include "Rendering/RenderContext.h"

#include <d3d12.h>

namespace PhantomCore
{
class FrameImageReadbackQueue;

// Engine-owned recording state, borrowed for one BeginFrame/EndFrame pair.
struct RenderFrameContext final
{
    ID3D12GraphicsCommandList* commandList{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissorRectangle{};
    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView{};
    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView{};
    std::uint64_t renderIndex{};
    FrameImageReadbackQueue* imageReadbacks{};

    void Attach(RenderContext& context) const noexcept
    {
        context.frame_ = this;
    }

    [[nodiscard]] static const RenderFrameContext* Find(
        const RenderContext& context) noexcept
    {
        const auto* frame = context.frame_;
        return frame != nullptr && frame->renderIndex == context.renderIndex
            ? frame : nullptr;
    }
};

} // namespace PhantomCore
