#pragma once

#include <fmod_errors.h>

#include <string>

namespace FmodDetail
{
    [[nodiscard]] inline std::string MakeFmodError(const char* operation, const FMOD_RESULT result)
    {
        return std::string(operation) + " failed: " +
            FMOD_ErrorString(result) + " (" +
            std::to_string(static_cast<int>(result)) + ")";
    }
}
