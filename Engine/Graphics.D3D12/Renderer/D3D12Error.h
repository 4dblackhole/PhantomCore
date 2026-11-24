#pragma once

#include "Diagnostics/EngineDiagnostics.h"

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

    // The results a call returns once the device was removed, hung or reset.
    [[nodiscard]] inline bool IsDeviceLoss(const HRESULT result) noexcept
    {
        switch (result)
        {
        case DXGI_ERROR_DEVICE_REMOVED:
        case DXGI_ERROR_DEVICE_HUNG:
        case DXGI_ERROR_DEVICE_RESET:
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            return true;
        default:
            return false;
        }
    }

    // operation is a string literal: a lost device keeps the pointer.
    inline void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (!FAILED(result))
            return;
        std::string message = std::string(operation) + " failed with HRESULT " + FormatHResult(result) + ".";
        if (IsDeviceLoss(result))
            throw GraphicsDeviceLostError(operation, static_cast<std::uint32_t>(result), message);
        throw std::runtime_error(message);
    }
}

} // namespace PhantomCore
