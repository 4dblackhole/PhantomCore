#include "System/AudioPlaybackManager.h"

#include <utility>


namespace PhantomCore
{
AudioPlaybackManager::~AudioPlaybackManager()
{
    StopAll();
}

AudioPlaybackId AudioPlaybackManager::Play(
    std::shared_ptr<AudioClip> clip,
    const AudioPlaybackSettings& settings,
    std::shared_ptr<AudioBus> bus,
    std::string& errorMessage)
{
    if (clip == nullptr || !clip->IsValid() ||
        (bus != nullptr && !bus->IsValid()))
    {
        errorMessage = "Playback requires a valid clip and optional bus.";
        return InvalidAudioPlaybackId;
    }
    if (nextId_ == InvalidAudioPlaybackId)
    {
        errorMessage = "Audio playback IDs are exhausted.";
        return InvalidAudioPlaybackId;
    }

    // Allocate the owning entry before starting native playback.
    const AudioPlaybackId id = nextId_++;
    auto [entry, inserted] = playbacks_.emplace(
        id, Playback{std::move(clip), std::move(bus), nullptr});
    static_cast<void>(inserted);
    try
    {
        Playback& playback = entry->second;
        playback.voice = playback.clip->Play(
            settings, playback.bus.get(), errorMessage);
        if (playback.voice == nullptr)
        {
            playbacks_.erase(entry);
            return InvalidAudioPlaybackId;
        }
    }
    catch (...)
    {
        playbacks_.erase(entry);
        throw;
    }
    errorMessage.clear();
    return id;
}

AudioVoice* AudioPlaybackManager::FindVoice(const AudioPlaybackId id) noexcept
{
    const auto entry = playbacks_.find(id);
    return entry == playbacks_.end() ? nullptr : entry->second.voice.get();
}

const AudioVoice* AudioPlaybackManager::FindVoice(
    const AudioPlaybackId id) const noexcept
{
    const auto entry = playbacks_.find(id);
    return entry == playbacks_.end() ? nullptr : entry->second.voice.get();
}

bool AudioPlaybackManager::Restart(
    const AudioPlaybackId id,
    const AudioPlaybackSettings& settings,
    std::shared_ptr<AudioBus> bus,
    std::string& errorMessage)
{
    const auto entry = playbacks_.find(id);
    if (entry == playbacks_.end())
    {
        errorMessage = "The audio playback ID is no longer active.";
        return false;
    }
    if (bus != nullptr && !bus->IsValid())
    {
        errorMessage = "Playback restart requires a valid optional bus.";
        return false;
    }
    auto& playback = entry->second;
    const bool wasPlaying = playback.voice->IsPlaying();
    if (wasPlaying && playback.voice->Restart(settings, bus.get(), errorMessage))
    {
        playback.bus = std::move(bus);
        errorMessage.clear();
        return true;
    }
    // The mixer may finish or steal a sample after IsPlaying. Recreate only
    // an ended voice; preserve errors on a still-controllable channel.
    if (playback.voice->IsPlaying())
        return false;
    auto replacement = playback.clip->Play(settings, bus.get(), errorMessage);
    if (!replacement)
        return false;
    playback.voice = std::move(replacement);
    playback.bus = std::move(bus);
    errorMessage.clear();
    return true;
}

bool AudioPlaybackManager::Stop(
    const AudioPlaybackId id, std::string& errorMessage)
{
    const auto entry = playbacks_.find(id);
    if (entry == playbacks_.end())
    {
        errorMessage = "The audio playback ID is no longer active.";
        return false;
    }
    AudioVoice& voice = *entry->second.voice;
    if (voice.IsPlaying() && !voice.Stop(errorMessage) && voice.IsPlaying())
    {
        return false;
    }
    playbacks_.erase(entry);
    errorMessage.clear();
    return true;
}

void AudioPlaybackManager::StopAll() noexcept
{
    for (auto& [id, playback] : playbacks_)
    {
        static_cast<void>(id);
        try
        {
            std::string ignoredError;
            if (playback.voice->IsPlaying())
            {
                static_cast<void>(playback.voice->Stop(ignoredError));
            }
        }
        catch (...)
        {
            // Shutdown must release all resources even if a backend throws.
        }
    }
    playbacks_.clear();
}

void AudioPlaybackManager::Update()
{
    std::erase_if(playbacks_, [](const auto& entry)
    {
        std::string error;
        // Progress async Play even for callers that only use the manager.
        // Prepared rhythm transports also poll explicitly before scheduling.
        static_cast<void>(entry.second.voice->PreparationState(error));
        return !entry.second.voice->IsPlaying();
    });
}

std::size_t AudioPlaybackManager::PlaybackCount() const noexcept
{
    return playbacks_.size();
}

} // namespace PhantomCore
