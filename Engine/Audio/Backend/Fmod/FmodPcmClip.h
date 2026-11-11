#pragma once
#include "FmodAudioBackend.h"

namespace PhantomCore
{
[[nodiscard]] std::unique_ptr<IAudioClipBackend> CreateFmodPcmClip(
    FmodAudioBackend& backend, const std::filesystem::path& path,
    AudioLoadMode mode, std::string& error);

} // namespace PhantomCore
