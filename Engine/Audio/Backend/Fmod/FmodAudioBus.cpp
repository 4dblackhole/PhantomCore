#include "FmodAudioBus.h"
#include "Backend/Fmod/FmodError.h"

#include <fmod_dsp_effects.h>
#include <fmod_errors.h>

#include <array>
#include <string>
#include <utility>


namespace
{
    [[nodiscard]] FMOD_DSP_TYPE ToFmodEffectType(
        const AudioEffectType type) noexcept
    {
        switch (type)
        {
        case AudioEffectType::LowPass:
            return FMOD_DSP_TYPE_LOWPASS;
        case AudioEffectType::HighPass:
            return FMOD_DSP_TYPE_HIGHPASS;
        case AudioEffectType::Compressor:
            return FMOD_DSP_TYPE_COMPRESSOR;
        case AudioEffectType::Delay:
            return FMOD_DSP_TYPE_DELAY;
        case AudioEffectType::Reverb:
            return FMOD_DSP_TYPE_SFXREVERB;
        }
        return FMOD_DSP_TYPE_UNKNOWN;
    }

    [[nodiscard]] bool ApplyChannelControlResult(
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
}

FmodBusLifetime::~FmodBusLifetime()
{
    if (group != nullptr && IsValid())
    {
        static_cast<void>(group->release());
    }
    group = nullptr;
}

bool FmodBusLifetime::IsValid() const noexcept
{
    return group != nullptr &&
        systemLifetime != nullptr &&
        systemLifetime->system != nullptr &&
        systemLifetime->generation == generation;
}

FmodAudioEffect::FmodAudioEffect(
    std::shared_ptr<FmodBusLifetime> busLifetime,
    FMOD::DSP* const dsp,
    const AudioEffectType type) noexcept
    : busLifetime_(std::move(busLifetime)),
      dsp_(dsp),
      type_(type),
      generation_(
          busLifetime_ != nullptr ? busLifetime_->generation : 0)
{
}

FmodAudioEffect::~FmodAudioEffect()
{
    if (dsp_ != nullptr && HasLiveSystem())
    {
        if (busLifetime_->group != nullptr)
        {
            static_cast<void>(busLifetime_->group->removeDSP(dsp_));
        }
        static_cast<void>(dsp_->release());
    }
    dsp_ = nullptr;
}

AudioEffectType FmodAudioEffect::Type() const noexcept
{
    return type_;
}

bool FmodAudioEffect::SetBypass(
    const bool bypass,
    std::string& errorMessage)
{
    if (dsp_ == nullptr || !HasLiveSystem())
    {
        errorMessage = "The FMOD effect is no longer valid.";
        return false;
    }
    return ApplyChannelControlResult(
        "FMOD::DSP::setBypass",
        dsp_->setBypass(bypass),
        errorMessage);
}

bool FmodAudioEffect::SetParameter(
    const AudioEffectParameter parameter,
    const float value,
    std::string& errorMessage)
{
    if (dsp_ == nullptr || !HasLiveSystem())
    {
        errorMessage = "The FMOD effect is no longer valid.";
        return false;
    }

    const int index = ParameterIndex(parameter);
    if (index < 0)
    {
        errorMessage = "The requested parameter is not supported by this effect.";
        return false;
    }
    return ApplyChannelControlResult(
        "FMOD::DSP::setParameterFloat",
        dsp_->setParameterFloat(index, value),
        errorMessage);
}

bool FmodAudioEffect::HasLiveSystem() const noexcept
{
    return busLifetime_ != nullptr &&
        busLifetime_->IsValid() &&
        busLifetime_->generation == generation_;
}

int FmodAudioEffect::ParameterIndex(
    const AudioEffectParameter parameter) const noexcept
{
    switch (type_)
    {
    case AudioEffectType::LowPass:
        if (parameter == AudioEffectParameter::CutoffHz)
        {
            return FMOD_DSP_LOWPASS_CUTOFF;
        }
        if (parameter == AudioEffectParameter::Resonance)
        {
            return FMOD_DSP_LOWPASS_RESONANCE;
        }
        break;
    case AudioEffectType::HighPass:
        if (parameter == AudioEffectParameter::CutoffHz)
        {
            return FMOD_DSP_HIGHPASS_CUTOFF;
        }
        if (parameter == AudioEffectParameter::Resonance)
        {
            return FMOD_DSP_HIGHPASS_RESONANCE;
        }
        break;
    case AudioEffectType::Compressor:
        if (parameter == AudioEffectParameter::ThresholdDb)
        {
            return FMOD_DSP_COMPRESSOR_THRESHOLD;
        }
        if (parameter == AudioEffectParameter::Ratio)
        {
            return FMOD_DSP_COMPRESSOR_RATIO;
        }
        if (parameter == AudioEffectParameter::AttackMilliseconds)
        {
            return FMOD_DSP_COMPRESSOR_ATTACK;
        }
        if (parameter == AudioEffectParameter::ReleaseMilliseconds)
        {
            return FMOD_DSP_COMPRESSOR_RELEASE;
        }
        break;
    case AudioEffectType::Delay:
        if (parameter == AudioEffectParameter::DelayMilliseconds)
        {
            return FMOD_DSP_DELAY_CH0;
        }
        break;
    case AudioEffectType::Reverb:
        if (parameter == AudioEffectParameter::DecayTimeMilliseconds)
        {
            return FMOD_DSP_SFXREVERB_DECAYTIME;
        }
        if (parameter == AudioEffectParameter::EarlyDelayMilliseconds)
        {
            return FMOD_DSP_SFXREVERB_EARLYDELAY;
        }
        if (parameter == AudioEffectParameter::LateDelayMilliseconds)
        {
            return FMOD_DSP_SFXREVERB_LATEDELAY;
        }
        if (parameter == AudioEffectParameter::WetLevelDb)
        {
            return FMOD_DSP_SFXREVERB_WETLEVEL;
        }
        if (parameter == AudioEffectParameter::DryLevelDb)
        {
            return FMOD_DSP_SFXREVERB_DRYLEVEL;
        }
        if (parameter == AudioEffectParameter::DiffusionPercent)
        {
            return FMOD_DSP_SFXREVERB_DIFFUSION;
        }
        if (parameter == AudioEffectParameter::DensityPercent)
        {
            return FMOD_DSP_SFXREVERB_DENSITY;
        }
        break;
    }
    return -1;
}

FmodAudioBus::FmodAudioBus(
    std::shared_ptr<FmodSystemLifetime> lifetime,
    FMOD::ChannelGroup* const group,
    std::string name) noexcept
    : busLifetime_(std::make_shared<FmodBusLifetime>()),
      name_(std::move(name))
{
    busLifetime_->generation = lifetime != nullptr
        ? lifetime->generation
        : 0;
    busLifetime_->systemLifetime = std::move(lifetime);
    busLifetime_->group = group;
}

FmodAudioBus::~FmodAudioBus() = default;

std::string_view FmodAudioBus::Name() const noexcept
{
    return name_;
}

bool FmodAudioBus::SetVolume(
    const float volume,
    std::string& errorMessage)
{
    if (!HasLiveSystem())
    {
        errorMessage = "The FMOD bus is no longer valid.";
        return false;
    }
    return ApplyChannelControlResult(
        "FMOD::ChannelGroup::setVolume",
        busLifetime_->group->setVolume(volume),
        errorMessage);
}

bool FmodAudioBus::SetPitch(
    const float pitch,
    std::string& errorMessage)
{
    if (!HasLiveSystem())
    {
        errorMessage = "The FMOD bus is no longer valid.";
        return false;
    }
    return ApplyChannelControlResult(
        "FMOD::ChannelGroup::setPitch",
        busLifetime_->group->setPitch(pitch),
        errorMessage);
}

bool FmodAudioBus::SetMuted(
    const bool muted,
    std::string& errorMessage)
{
    if (!HasLiveSystem())
    {
        errorMessage = "The FMOD bus is no longer valid.";
        return false;
    }
    return ApplyChannelControlResult(
        "FMOD::ChannelGroup::setMute",
        busLifetime_->group->setMute(muted),
        errorMessage);
}

bool FmodAudioBus::AddFadePoint(
    const std::uint64_t dspClock,
    const float volume,
    const bool ramp,
    std::string& errorMessage)
{
    if (!HasLiveSystem())
    {
        errorMessage = "The FMOD bus is no longer valid.";
        return false;
    }
    const FMOD_RESULT result = ramp
        ? busLifetime_->group->setFadePointRamp(dspClock, volume)
        : busLifetime_->group->addFadePoint(dspClock, volume);
    return ApplyChannelControlResult(
        ramp
            ? "FMOD::ChannelGroup::setFadePointRamp"
            : "FMOD::ChannelGroup::addFadePoint",
        result,
        errorMessage);
}

bool FmodAudioBus::ClearFadePoints(
    const std::uint64_t beginDspClock,
    const std::uint64_t endDspClock,
    std::string& errorMessage)
{
    if (!HasLiveSystem())
    {
        errorMessage = "The FMOD bus is no longer valid.";
        return false;
    }
    return ApplyChannelControlResult(
        "FMOD::ChannelGroup::removeFadePoints",
        busLifetime_->group->removeFadePoints(beginDspClock, endDspClock),
        errorMessage);
}

std::unique_ptr<IAudioEffectBackend> FmodAudioBus::AddEffect(
    const AudioEffectType type,
    std::string& errorMessage)
{
    if (!HasLiveSystem())
    {
        errorMessage = "The FMOD bus is no longer valid.";
        return nullptr;
    }

    FMOD::DSP* dsp = nullptr;
    FMOD_RESULT result = busLifetime_->systemLifetime->system->createDSPByType(
        ToFmodEffectType(type),
        &dsp);
    if (result != FMOD_OK || dsp == nullptr)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::System::createDSPByType", result);
        return nullptr;
    }

    result = busLifetime_->group->addDSP(0, dsp);
    if (result != FMOD_OK)
    {
        static_cast<void>(dsp->release());
        errorMessage = FmodDetail::MakeFmodError("FMOD::ChannelGroup::addDSP", result);
        return nullptr;
    }

    errorMessage.clear();
    return std::make_unique<FmodAudioEffect>(
        busLifetime_,
        dsp,
        type);
}

FMOD::ChannelGroup* FmodAudioBus::NativeGroup() const noexcept
{
    return HasLiveSystem() ? busLifetime_->group : nullptr;
}

const std::shared_ptr<FmodBusLifetime>&
FmodAudioBus::SharedLifetime() const noexcept
{
    return busLifetime_;
}

bool FmodAudioBus::HasLiveSystem() const noexcept
{
    return busLifetime_ != nullptr && busLifetime_->IsValid();
}
