#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace PhantomCore
{
namespace Visual2DDetail
{
    [[nodiscard]] inline std::wstring ImageCacheKey(const std::filesystem::path& path)
    {
        std::wstring key = path.lexically_normal().generic_wstring();
        std::ranges::transform(key, key.begin(), [](const wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return key;
    }
}

} // namespace PhantomCore
