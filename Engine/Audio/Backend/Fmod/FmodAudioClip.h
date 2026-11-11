#pragma once

#include "FmodAudioBackend.h"

#include <fmod.hpp>

#include <cstdint>
#include <memory>


// Owns one FMOD sound for as long as the Client-owned AudioClip exists.
namespace PhantomCore
{
class FmodAudioClip final : public IAudioClipBackend
{
public:
    FmodAudioClip(
        std::shared_ptr<FmodSystemLifetime> lifetime,
        FMOD::Sound* sound) noexcept;
    ~FmodAudioClip() override;
    AudioClipLoadState LoadState(std::string& errorMessage) const override;
    bool GetDurationMicroseconds(std::uint64_t& duration, std::string& errorMessage) const override;

    [[nodiscard]] std::unique_ptr<IAudioVoiceBackend> Play(
        const AudioPlaybackSettings& settings,
        IAudioBusBackend* bus,
        std::string& errorMessage) override;

private:
    [[nodiscard]] bool HasLiveSystem() const noexcept;

    std::shared_ptr<FmodSystemLifetime> lifetime_;
    FMOD::Sound* sound_{};
    std::uint64_t generation_{};
};

} // namespace PhantomCore
