#pragma once

// Process-relative path helpers owned by the Win32 platform layer. Client
// code can locate packaged assets without depending on its working directory
// or calling Win32 APIs directly.

#include <filesystem>

namespace PhantomCore
{
[[nodiscard]] std::filesystem::path ExecutablePath();
[[nodiscard]] std::filesystem::path ExecutableDirectory();

// Absolute paths pass through unchanged. Relative paths are resolved from
// the directory containing the current process executable.
[[nodiscard]] std::filesystem::path ResolveExecutableRelativePath(
    const std::filesystem::path& path);

} // namespace PhantomCore
