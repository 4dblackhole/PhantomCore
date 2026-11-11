#include "System/RuntimePaths.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace PhantomCore
{
namespace
{
    constexpr std::size_t InitialPathCapacity = MAX_PATH;
    constexpr std::size_t MaximumExtendedPathCapacity = 32768;
}

std::filesystem::path ExecutablePath()
{
    // Most paths fit in MAX_PATH. Grow only when GetModuleFileNameW
    // reports truncation instead of reserving about 64 KiB on every call.
    std::wstring buffer(InitialPathCapacity, L'\0');
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            const DWORD error = GetLastError();
            throw std::system_error(
                static_cast<int>(
                    error != ERROR_SUCCESS
                        ? error
                        : ERROR_GEN_FAILURE),
                std::system_category(),
                "GetModuleFileNameW failed");
        }

        if (length < buffer.size())
        {
            buffer.resize(length);
            return std::filesystem::path(std::move(buffer));
        }

        if (buffer.size() >= MaximumExtendedPathCapacity)
        {
            throw std::length_error(
                "The executable path exceeds the supported Win32 "
                "extended-path capacity.");
        }

        buffer.resize(std::min(
            buffer.size() * 2U,
            MaximumExtendedPathCapacity));
    }
}

std::filesystem::path ExecutableDirectory()
{
    return ExecutablePath().parent_path();
}

std::filesystem::path ResolveExecutableRelativePath(
    const std::filesystem::path& path)
{
    return path.is_absolute()
        ? path
        : ExecutableDirectory() / path;
}

} // namespace PhantomCore
