#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace PhantomCore
{
namespace D3D12Detail
{
    // Every D3D12, DXGI, DirectWrite and WIC failure reports the HRESULT in the
    // same eight-digit hexadecimal form used by Microsoft documentation.
    [[nodiscard]] inline std::string FormatHResult(const HRESULT result)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string text = "0x00000000";
        const auto bits = static_cast<std::uint32_t>(result);
        for (std::size_t index = 0; index < 8; ++index)
        {
            const std::size_t shift = (7 - index) * 4;
            text[index + 2] = digits[(bits >> shift) & 0xF];
        }
        return text;
    }

    inline void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation) + " failed with HRESULT " +
                FormatHResult(result) + ".");
        }
    }
}

} // namespace PhantomCore
