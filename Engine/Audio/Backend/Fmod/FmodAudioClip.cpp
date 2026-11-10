#include "FmodAudioClip.h"
#include "Backend/Fmod/FmodError.h"

#include "FmodAudioBus.h"
#include "FmodPcmClip.h"

#include <fmod_errors.h>

#include <filesystem>
#include <cmath>
#include <limits>
#include <string>
#include <utility>


namespace
{
    FMOD_RESULT SoundTiming(FMOD::Sound* sound, unsigned int& frames, float& frequency)
    {
        auto result = sound->getLength(&frames, FMOD_TIMEUNIT_PCM);
        if (result == FMOD_OK)
            result = sound->getDefaults(&frequency, nullptr);
        if (result == FMOD_OK && (!std::isfinite(frequency) || frequency <= 0 ||
                                 frames == std::numeric_limits<unsigned int>::max()))
            result = FMOD_ERR_FORMAT;
        return result;
    }

    FMOD_RESULT SetSourcePosition(FMOD::Channel* channel, const std::uint64_t microseconds)
    {
        if (microseconds == 0)
            return channel->setPosition(0, FMOD_TIMEUNIT_PCM);
        FMOD::Sound* sound = nullptr;
        auto result = channel->getCurrentSound(&sound);
        if (result != FMOD_OK || !sound)
            return result != FMOD_OK ? result : FMOD_ERR_INVALID_HANDLE;
        unsigned int frames = 0;
        float frequency = 0;
        result = SoundTiming(sound, frames, frequency);
        if (result != FMOD_OK)
            return result;
        long double position = std::round(static_cast<long double>(microseconds) * frequency / 1'000'000);
        if (position >= frames)
        {
            const auto duration = std::round(static_cast<long double>(frames) * 1'000'000 / frequency);
            if (frames == 0 || static_cast<long double>(microseconds) >= duration)
                return FMOD_ERR_INVALID_POSITION;
            position = frames - 1; // A valid end-adjacent time rounds to the last sample.
        }
        return channel->setPosition(static_cast<unsigned int>(position), FMOD_TIMEUNIT_PCM);
    }

    class FmodAudioVoice final : public IAudioVoiceBackend
    {
    public:
        FmodAudioVoice(
            std::shared_ptr<FmodSystemLifetime> lifetime,
            FMOD::Channel* const channel, FMOD::Sound* const sound) noexcept
            : lifetime_(std::move(lifetime)),
              channel_(channel),
              sound_(sound),
              generation_(
                  lifetime_ != nullptr ? lifetime_->generation : 0)
        {
        }

        bool BeginSeek(std::uint64_t position, bool paused, std::string& error)
        {
            sourcePosition_ = position;
            pausedAfterSeek_ = paused;
            seekStage_ = SeekStage::AwaitSource;
            sourceReady_ = false;
            preparationError_.clear();
            return PreparationState(error) != AudioPreparationState::Failed;
        }

        AudioPreparationState PreparationState(std::string& error) override
        {
            if (!preparationError_.empty())
            {
                error = preparationError_;
                return AudioPreparationState::Failed;
            }
            if (!HasLiveSystem())
                return FailPreparation(FMOD_ERR_INVALID_HANDLE, error);
            if (sourceReady_ && seekStage_ == SeekStage::None)
            {
                error.clear();
                return AudioPreparationState::Ready;
            }
            auto state = SourceState(error);
            if (state != AudioPreparationState::Ready)
                return state;
            if (seekStage_ == SeekStage::AwaitSource)
            {
                const auto result = SetSourcePosition(channel_, sourcePosition_);
                if (result == FMOD_ERR_NOTREADY)
                {
                    error.clear();
                    return AudioPreparationState::Loading;
                }
                if (result != FMOD_OK)
                    return FailPreparation(result, error);
                seekStage_ = SeekStage::AwaitSeek;
                state = SourceState(error);
                if (state != AudioPreparationState::Ready)
                    return state;
            }
            if (seekStage_ == SeekStage::AwaitSeek)
            {
                const auto result = channel_->setPaused(pausedAfterSeek_);
                if (result != FMOD_OK)
                    return FailPreparation(result, error);
                seekStage_ = SeekStage::None;
            }
            error.clear();
            sourceReady_ = true;
            return AudioPreparationState::Ready;
        }

        [[nodiscard]] bool IsPlaying() const noexcept override
        {
            if (!HasLiveSystem())
            {
                return false;
            }
            bool isPlaying = false;
            return channel_->isPlaying(&isPlaying) == FMOD_OK && isPlaying;
        }

        [[nodiscard]] bool Restart(
            const AudioPlaybackSettings& settings,
            IAudioBusBackend* const bus,
            std::string& errorMessage) override
        {
            if (!HasLiveSystem())
            {
                return Apply(
                    "FMOD::Channel::setPosition",
                    FMOD_ERR_INVALID_HANDLE,
                    errorMessage);
            }

            FMOD::ChannelGroup* nativeBus = nullptr;
            if (bus != nullptr)
            {
                auto* const fmodBus = dynamic_cast<FmodAudioBus*>(bus);
                if (fmodBus == nullptr || !fmodBus->HasLiveSystem())
                {
                    errorMessage =
                        "The audio bus belongs to another backend or is invalid.";
                    return false;
                }
                nativeBus = fmodBus->NativeGroup();
            }
            else
            {
                const FMOD_RESULT masterResult =
                    lifetime_->system->getMasterChannelGroup(&nativeBus);
                if (masterResult != FMOD_OK)
                {
                    return Apply(
                        "FMOD::System::getMasterChannelGroup",
                        masterResult,
                        errorMessage);
                }
                if (nativeBus == nullptr)
                {
                    errorMessage =
                        "FMOD::System::getMasterChannelGroup returned no bus.";
                    return false;
                }
            }

            if (!Apply(
                    "FMOD::Channel::setPaused",
                    channel_->setPaused(true),
                    errorMessage) ||
                !Apply(
                    "FMOD::Channel::setChannelGroup",
                    channel_->setChannelGroup(nativeBus),
                    errorMessage) ||
                !Apply(
                    "FMOD::Channel::setVolume",
                    channel_->setVolume(settings.volume),
                    errorMessage) ||
                !Apply(
                    "FMOD::Channel::setPitch",
                    channel_->setPitch(settings.pitch),
                    errorMessage) ||
                !Apply(
                    "FMOD::Channel::setDelay",
                    channel_->setDelay(
                        settings.startDspClock,
                        settings.endDspClock,
                        settings.endDspClock != 0),
                    errorMessage))
            {
                return false;
            }

            return BeginSeek(settings.startPositionMicroseconds, settings.startPaused, errorMessage);
        }

        [[nodiscard]] bool Stop(
            std::string& errorMessage) override
        {
            seekStage_ = SeekStage::None;
            return Apply(
                "FMOD::Channel::stop",
                HasLiveSystem() ? channel_->stop() : FMOD_ERR_INVALID_HANDLE,
                errorMessage);
        }

        [[nodiscard]] bool SetPaused(
            const bool paused,
            std::string& errorMessage) override
        {
            if (seekStage_ != SeekStage::None && HasLiveSystem())
            {
                pausedAfterSeek_ = paused;
                // A pending seek must never become audible before Ready.
                return Apply("FMOD::Channel::setPaused", channel_->setPaused(true), errorMessage);
            }
            return Apply(
                "FMOD::Channel::setPaused",
                HasLiveSystem()
                    ? channel_->setPaused(paused)
                    : FMOD_ERR_INVALID_HANDLE,
                errorMessage);
        }

        [[nodiscard]] bool SetVolume(
            const float volume,
            std::string& errorMessage) override
        {
            return Apply(
                "FMOD::Channel::setVolume",
                HasLiveSystem()
                    ? channel_->setVolume(volume)
                    : FMOD_ERR_INVALID_HANDLE,
                errorMessage);
        }

        [[nodiscard]] bool SetPitch(
            const float pitch,
            std::string& errorMessage) override
        {
            return Apply(
                "FMOD::Channel::setPitch",
                HasLiveSystem()
                    ? channel_->setPitch(pitch)
                    : FMOD_ERR_INVALID_HANDLE,
                errorMessage);
        }

        bool SetDelay(const std::uint64_t start, const std::uint64_t end, std::string& error) override
        {
            return Apply("FMOD::Channel::setDelay", HasLiveSystem()
                ? channel_->setDelay(start, end, end != 0) : FMOD_ERR_INVALID_HANDLE, error);
        }

    private:
        enum class SeekStage { None, AwaitSource, AwaitSeek };

        AudioPreparationState FailPreparation(FMOD_RESULT result, std::string& error)
        {
            preparationError_ = FmodDetail::MakeFmodError("FMOD voice preparation", result);
            error = preparationError_;
            return AudioPreparationState::Failed;
        }

        AudioPreparationState SourceState(std::string& error)
        {
            FMOD_OPENSTATE state{};
            const auto result = sound_->getOpenState(&state, nullptr, nullptr, nullptr);
            if (result != FMOD_OK || state == FMOD_OPENSTATE_ERROR)
                return FailPreparation(result != FMOD_OK ? result : FMOD_ERR_INTERNAL, error);
            error.clear();
            return state == FMOD_OPENSTATE_READY || state == FMOD_OPENSTATE_PLAYING
                ? AudioPreparationState::Ready : AudioPreparationState::Loading;
        }

        [[nodiscard]] bool HasLiveSystem() const noexcept
        {
            return channel_ != nullptr &&
                lifetime_ != nullptr &&
                lifetime_->system != nullptr &&
                lifetime_->generation == generation_;
        }

        [[nodiscard]] static bool Apply(
            const char* operation,
            const FMOD_RESULT result,
            std::string& errorMessage)
        {
            if (result != FMOD_OK)
            {
                errorMessage = FmodDetail::MakeFmodError(operation, result);
                return false;
            }
            errorMessage.clear();
            return true;
        }

        std::shared_ptr<FmodSystemLifetime> lifetime_;
        FMOD::Channel* channel_{};
        FMOD::Sound* sound_{}; // The caller/PlaybackManager retains the clip.
        std::uint64_t generation_{};
        SeekStage seekStage_{};
        std::uint64_t sourcePosition_{};
        bool pausedAfterSeek_{};
        bool sourceReady_{};
        std::string preparationError_;
    };
}

FmodAudioClip::FmodAudioClip(
    std::shared_ptr<FmodSystemLifetime> lifetime,
    FMOD::Sound* sound) noexcept
    : lifetime_(std::move(lifetime)),
      sound_(sound),
      generation_(lifetime_ != nullptr ? lifetime_->generation : 0)
{
}

FmodAudioClip::~FmodAudioClip()
{
    // System::close already invalidates and frees its child objects. Only
    // call release while this sound still belongs to the live generation.
    if (sound_ != nullptr && HasLiveSystem())
    {
        static_cast<void>(sound_->release());
    }
    sound_ = nullptr;
}

std::unique_ptr<IAudioVoiceBackend> FmodAudioClip::Play(
    const AudioPlaybackSettings& settings,
    IAudioBusBackend* const bus,
    std::string& errorMessage)
{
    if (sound_ == nullptr || !HasLiveSystem())
    {
        errorMessage = "The FMOD sound is no longer valid.";
        return nullptr;
    }

    // Validate against the loaded source before playSound can initiate an
    // implicit async rewind. Invalid requests must not allocate a channel.
    if (settings.startPositionMicroseconds != 0)
    {
        std::uint64_t duration{};
        if (!GetDurationMicroseconds(duration, errorMessage))
            return nullptr;
        if (settings.startPositionMicroseconds >= duration)
        {
            errorMessage = FmodDetail::MakeFmodError("FMOD source position", FMOD_ERR_INVALID_POSITION);
            return nullptr;
        }
    }

    FMOD::ChannelGroup* nativeBus = nullptr;
    if (bus != nullptr)
    {
        auto* const fmodBus = dynamic_cast<FmodAudioBus*>(bus);
        if (fmodBus == nullptr || !fmodBus->HasLiveSystem())
        {
            errorMessage =
                "The audio bus belongs to another backend or is invalid.";
            return nullptr;
        }
        nativeBus = fmodBus->NativeGroup();
    }

    FMOD::Channel* channel = nullptr;
    FMOD_RESULT result = lifetime_->system->playSound(
        sound_,
        nativeBus,
        true,
        &channel);
    if (result != FMOD_OK || channel == nullptr)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::System::playSound", result);
        return nullptr;
    }

    const auto failPlayback = [&](
        const char* operation,
        const FMOD_RESULT failure)
    {
        static_cast<void>(channel->stop());
        errorMessage = FmodDetail::MakeFmodError(operation, failure);
        return std::unique_ptr<IAudioVoiceBackend>{};
    };

    // A new voice already starts at zero. Avoid flushing an async stream
    // unless the caller explicitly requested another source position.
    result = channel->setVolume(settings.volume);
    if (result != FMOD_OK)
    {
        return failPlayback("FMOD::Channel::setVolume", result);
    }
    result = channel->setPitch(settings.pitch);
    if (result != FMOD_OK)
    {
        return failPlayback("FMOD::Channel::setPitch", result);
    }
    if (settings.startDspClock != 0 || settings.endDspClock != 0)
    {
        result = channel->setDelay(
            settings.startDspClock,
            settings.endDspClock,
            settings.endDspClock != 0);
        if (result != FMOD_OK)
        {
            return failPlayback("FMOD::Channel::setDelay", result);
        }
    }
    auto voice = std::make_unique<FmodAudioVoice>(lifetime_, channel, sound_);
    if (settings.startPositionMicroseconds != 0)
    {
        if (!voice->BeginSeek(settings.startPositionMicroseconds, settings.startPaused, errorMessage))
        {
            static_cast<void>(channel->stop());
            return nullptr;
        }
        return voice;
    }
    result = channel->setPaused(settings.startPaused);
    if (result != FMOD_OK)
    {
        return failPlayback("FMOD::Channel::setPaused", result);
    }

    errorMessage.clear();
    return voice;
}

AudioClipLoadState FmodAudioClip::LoadState(std::string& errorMessage) const
{
    if (!sound_ || !HasLiveSystem())
    {
        errorMessage = "The audio clip no longer belongs to a live audio system.";
        return AudioClipLoadState::Failed;
    }
    FMOD_OPENSTATE state{};
    const auto result = sound_->getOpenState(&state, nullptr, nullptr, nullptr);
    if (result != FMOD_OK || state == FMOD_OPENSTATE_ERROR)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::Sound::getOpenState", result);
        return AudioClipLoadState::Failed;
    }
    errorMessage.clear();
    if (state == FMOD_OPENSTATE_READY)
        return AudioClipLoadState::Ready;
    if (state == FMOD_OPENSTATE_PLAYING)
        return AudioClipLoadState::Playing;
    return AudioClipLoadState::Loading;
}

bool FmodAudioClip::GetDurationMicroseconds(std::uint64_t& duration, std::string& error) const
{
    duration = 0;
    if (!sound_ || !HasLiveSystem())
    {
        error = "The audio clip no longer belongs to a live audio system.";
        return false;
    }
    unsigned int frames = 0;
    float frequency = 0;
    const auto result = SoundTiming(sound_, frames, frequency);
    if (result != FMOD_OK)
    {
        error = FmodDetail::MakeFmodError("FMOD sound timing", result);
        return false;
    }
    const long double microseconds = std::round(static_cast<long double>(frames) * 1'000'000 / frequency);
    if (microseconds >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
    {
        error = "The audio duration exceeds the microsecond range.";
        return false;
    }
    duration = static_cast<std::uint64_t>(microseconds);
    error.clear();
    return true;
}

bool FmodAudioClip::HasLiveSystem() const noexcept
{
    return lifetime_ != nullptr &&
        lifetime_->system != nullptr &&
        lifetime_->generation == generation_;
}

std::unique_ptr<IAudioClipBackend> CreateFmodAudioClipBackend(
    IAudioBackend& backend,
    const std::filesystem::path& path,
    const AudioLoadMode loadMode,
    std::string& errorMessage)
{
    auto* fmodBackend = dynamic_cast<FmodAudioBackend*>(&backend);
    if (fmodBackend == nullptr ||
        fmodBackend->NativeSystem() == nullptr ||
        !fmodBackend->IsMixerInitialized() ||
        fmodBackend->Lifetime() == nullptr)
    {
        errorMessage = "The FMOD backend is not initialized.";
        return nullptr;
    }

    return CreateFmodPcmClip(*fmodBackend, path, loadMode, errorMessage);
}
