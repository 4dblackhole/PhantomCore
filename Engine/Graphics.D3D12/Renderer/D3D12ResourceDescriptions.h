#pragma once

#include <d3d12.h>

#include <cstdint>

namespace PhantomCore
{
namespace D3D12Detail
{
    // Linear buffers used for uploads, readback and per-frame instances.
    [[nodiscard]] inline D3D12_RESOURCE_DESC BufferDescription(
        const std::uint64_t byteCount) noexcept
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = byteCount;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        return description;
    }
}

} // namespace PhantomCore
