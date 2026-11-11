#pragma once

#include "System/AudioSystem.h"

#include <map>


namespace PhantomCore
{
using AudioPlaybackId = std::uint64_t;
inline constexpr AudioPlaybackId InvalidAudioPlaybackId = 0;

// Game-wide playback ownership. Call on the Client thread. IDs belong to
// this manager and are never reused, including after StopAll().
class AudioPlaybackManager final
{
public:
    AudioPlaybackManager() = default;
    ~AudioPlaybackManager();
    AudioPlaybackManager(const AudioPlaybackManager&) = delete;
    AudioPlaybackManager& operator=(const AudioPlaybackManager&) = delete;

    // Retains the clip and optional bus until playback ends or is stopped.
    // The caller must keep any parent buses and attached effects alive.
    [[nodiscard]] AudioPlaybackId Play(
        std::shared_ptr<AudioClip> clip,
        const AudioPlaybackSettings& settings,
        std::shared_ptr<AudioBus> bus,
        std::string& errorMessage);
    // Borrowed pointer, invalidated by Stop, StopAll, a later Update, or a
    // Restart that replaces an ended channel. Reacquire it after Restart.
    [[nodiscard]] AudioVoice* FindVoice(AudioPlaybackId id) noexcept;
    [[nodiscard]] const AudioVoice* FindVoice(AudioPlaybackId id) const noexcept;
    // Reuses a live channel, or replaces an ended/stolen one under the same
    // ID, retaining the clip and new bus. Active-channel errors propagate.
    [[nodiscard]] bool Restart(
        AudioPlaybackId id,
        const AudioPlaybackSettings& settings,
        std::shared_ptr<AudioBus> bus,
        std::string& errorMessage);
    [[nodiscard]] bool Stop(AudioPlaybackId id, std::string& errorMessage);
    // Best-effort stop of every voice; also used during Client shutdown.
    void StopAll() noexcept;
    void Update();
    [[nodiscard]] std::size_t PlaybackCount() const noexcept;

private:
    struct Playback
    {
        std::shared_ptr<AudioClip> clip;
        std::shared_ptr<AudioBus> bus;
        std::unique_ptr<AudioVoice> voice;
    };
    std::map<AudioPlaybackId, Playback> playbacks_;
    AudioPlaybackId nextId_{1};
};

} // namespace PhantomCore
