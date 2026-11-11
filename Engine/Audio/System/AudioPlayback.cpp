#include "System/AudioPlayback.h"

#include <utility>


namespace PhantomCore
{
namespace
{
    void AcquireObject(
        const std::shared_ptr<std::atomic_size_t>& count) noexcept
    {
        if (count != nullptr)
        {
            count->fetch_add(1, std::memory_order_relaxed);
        }
    }

    void ReleaseObject(
        const std::shared_ptr<std::atomic_size_t>& count) noexcept
    {
        if (count != nullptr)
        {
            count->fetch_sub(1, std::memory_order_release);
        }
    }
}

AudioVoice::AudioVoice(
    std::unique_ptr<IAudioVoiceBackend> implementation,
    std::shared_ptr<std::atomic_size_t> liveObjectCount)
    : implementation_(std::move(implementation)),
      liveObjectCount_(std::move(liveObjectCount))
{
    AcquireObject(liveObjectCount_);
}

AudioVoice::~AudioVoice()
{
    implementation_.reset();
    ReleaseObject(liveObjectCount_);
}

bool AudioVoice::IsPlaying() const noexcept
{
    return implementation_ != nullptr && implementation_->IsPlaying();
}

AudioPreparationState AudioVoice::PreparationState(std::string& errorMessage)
{
    if (implementation_)
        return implementation_->PreparationState(errorMessage);
    errorMessage = "The audio voice is not initialized.";
    return AudioPreparationState::Failed;
}

bool AudioVoice::Restart(
    const AudioPlaybackSettings& settings,
    AudioBus* const bus,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio voice is not initialized.";
        return false;
    }
    return implementation_->Restart(
        settings,
        bus != nullptr ? bus->implementation_.get() : nullptr,
        errorMessage);
}

bool AudioVoice::Stop(std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio voice is not initialized.";
        return false;
    }
    return implementation_->Stop(errorMessage);
}

bool AudioVoice::SetPaused(
    const bool paused,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio voice is not initialized.";
        return false;
    }
    return implementation_->SetPaused(paused, errorMessage);
}

bool AudioVoice::SetVolume(
    const float volume,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio voice is not initialized.";
        return false;
    }
    return implementation_->SetVolume(volume, errorMessage);
}

bool AudioVoice::SetPitch(
    const float pitch,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio voice is not initialized.";
        return false;
    }
    return implementation_->SetPitch(pitch, errorMessage);
}

bool AudioVoice::SetDelay(const std::uint64_t startDspClock,
    const std::uint64_t endDspClock, std::string& errorMessage)
{
    if (!implementation_)
    {
        errorMessage = "The audio voice is not initialized.";
        return false;
    }
    return implementation_->SetDelay(startDspClock, endDspClock, errorMessage);
}

AudioEffect::AudioEffect(
    std::unique_ptr<IAudioEffectBackend> implementation,
    std::shared_ptr<std::atomic_size_t> liveObjectCount)
    : implementation_(std::move(implementation)),
      liveObjectCount_(std::move(liveObjectCount))
{
    AcquireObject(liveObjectCount_);
}

AudioEffect::~AudioEffect()
{
    implementation_.reset();
    ReleaseObject(liveObjectCount_);
}

bool AudioEffect::IsValid() const noexcept
{
    return implementation_ != nullptr;
}

AudioEffectType AudioEffect::Type() const noexcept
{
    return implementation_ != nullptr
        ? implementation_->Type()
        : AudioEffectType::LowPass;
}

bool AudioEffect::SetBypass(
    const bool bypass,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio effect is not initialized.";
        return false;
    }
    return implementation_->SetBypass(bypass, errorMessage);
}

bool AudioEffect::SetParameter(
    const AudioEffectParameter parameter,
    const float value,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio effect is not initialized.";
        return false;
    }
    return implementation_->SetParameter(parameter, value, errorMessage);
}

AudioBus::AudioBus(
    std::unique_ptr<IAudioBusBackend> implementation,
    std::shared_ptr<std::atomic_size_t> liveObjectCount)
    : implementation_(std::move(implementation)),
      liveObjectCount_(std::move(liveObjectCount))
{
    AcquireObject(liveObjectCount_);
}

AudioBus::~AudioBus()
{
    implementation_.reset();
    ReleaseObject(liveObjectCount_);
}

bool AudioBus::IsValid() const noexcept
{
    return implementation_ != nullptr;
}

std::string_view AudioBus::Name() const noexcept
{
    return implementation_ != nullptr
        ? implementation_->Name()
        : std::string_view{};
}

bool AudioBus::SetVolume(
    const float volume,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio bus is not initialized.";
        return false;
    }
    return implementation_->SetVolume(volume, errorMessage);
}

bool AudioBus::SetPitch(
    const float pitch,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio bus is not initialized.";
        return false;
    }
    return implementation_->SetPitch(pitch, errorMessage);
}

bool AudioBus::SetMuted(
    const bool muted,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio bus is not initialized.";
        return false;
    }
    return implementation_->SetMuted(muted, errorMessage);
}

bool AudioBus::AddFadePoint(
    const std::uint64_t dspClock,
    const float volume,
    const bool ramp,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio bus is not initialized.";
        return false;
    }
    return implementation_->AddFadePoint(
        dspClock,
        volume,
        ramp,
        errorMessage);
}

bool AudioBus::ClearFadePoints(
    const std::uint64_t beginDspClock,
    const std::uint64_t endDspClock,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio bus is not initialized.";
        return false;
    }
    return implementation_->ClearFadePoints(
        beginDspClock,
        endDspClock,
        errorMessage);
}

std::unique_ptr<AudioEffect> AudioBus::AddEffect(
    const AudioEffectType type,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio bus is not initialized.";
        return nullptr;
    }

    std::unique_ptr<IAudioEffectBackend> effect =
        implementation_->AddEffect(type, errorMessage);
    if (effect == nullptr)
    {
        return nullptr;
    }
    return std::unique_ptr<AudioEffect>(new AudioEffect(
        std::move(effect),
        liveObjectCount_));
}

} // namespace PhantomCore
