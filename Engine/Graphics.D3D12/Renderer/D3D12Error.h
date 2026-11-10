#pragma once

#include <Windows.h>

#include <stdexcept>
#include <string>

namespace D3D12Detail
{
    inline void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation) + " failed with HRESULT " +
                std::to_string(static_cast<long>(result)) + ".");
        }
    }
}
