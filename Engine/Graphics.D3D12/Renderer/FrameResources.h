#pragma once

#include <cstdint>

namespace PhantomCore
{
namespace D3D12Detail
{
    // One definition for swap-chain buffers and every fence-protected arena.
    inline constexpr std::uint32_t FrameCount = 2;

    // Draw calls recorded on this thread since D3D12Renderer::BeginFrame.
    // Every draw site increments it; the renderer reads it after EndFrame
    // for PerformanceStatistics. Recording happens on the render thread only.
    [[nodiscard]] inline std::uint32_t& RecordedDrawCalls() noexcept
    {
        thread_local std::uint32_t count{};
        return count;
    }
}

} // namespace PhantomCore
