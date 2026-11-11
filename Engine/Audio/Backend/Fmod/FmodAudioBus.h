#pragma once

#include "FmodAudioBackend.h"

#include <fmod.hpp>

#include <cstdint>
#include <memory>
#include <string>


namespace PhantomCore
{
struct FmodBusLifetime final
{
    ~FmodBusLifetime();

    std::shared_ptr<FmodSystemLifetime> systemLifetime;
    FMOD::ChannelGroup* group{};
    std::uint64_t generation{};

    [[nodiscard]] bool IsValid() const noexcept;
};

class FmodAudioEffect final : public IAudioEffectBackend
{
public:
    FmodAudioEffect(
        std::shared_ptr<FmodBusLifetime> busLifetime,
        FMOD::DSP* dsp,
        AudioEffectType type) noexcept;
    ~FmodAudioEffect() override;

    [[nodiscard]] AudioEffectType Type() const noexcept override;
    [[nodiscard]] bool SetBypass(
        bool bypass,
        std::string& errorMessage) override;
    [[nodiscard]] bool SetParameter(
        AudioEffectParameter parameter,
        float value,
        std::string& errorMessage) override;

private:
    [[nodiscard]] bool HasLiveSystem() const noexcept;
    [[nodiscard]] int ParameterIndex(
        AudioEffectParameter parameter) const noexcept;

    std::shared_ptr<FmodBusLifetime> busLifetime_;
    FMOD::DSP* dsp_{};
    AudioEffectType type_{};
    std::uint64_t generation_{};
};

class FmodAudioBus final : public IAudioBusBackend
{
public:
    FmodAudioBus(
        std::shared_ptr<FmodSystemLifetime> lifetime,
        FMOD::ChannelGroup* group,
        std::string name) noexcept;
    ~FmodAudioBus() override;

    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] bool SetVolume(
        float volume,
        std::string& errorMessage) override;
    [[nodiscard]] bool SetPitch(
        float pitch,
        std::string& errorMessage) override;
    [[nodiscard]] bool SetMuted(
        bool muted,
        std::string& errorMessage) override;
    [[nodiscard]] bool AddFadePoint(
        std::uint64_t dspClock,
        float volume,
        bool ramp,
        std::string& errorMessage) override;
    [[nodiscard]] bool ClearFadePoints(
        std::uint64_t beginDspClock,
        std::uint64_t endDspClock,
        std::string& errorMessage) override;
    [[nodiscard]] std::unique_ptr<IAudioEffectBackend> AddEffect(
        AudioEffectType type,
        std::string& errorMessage) override;

    [[nodiscard]] FMOD::ChannelGroup* NativeGroup() const noexcept;
    [[nodiscard]] const std::shared_ptr<FmodBusLifetime>&
        SharedLifetime() const noexcept;
    [[nodiscard]] bool HasLiveSystem() const noexcept;

private:
    std::shared_ptr<FmodBusLifetime> busLifetime_;
    std::string name_;
};

} // namespace PhantomCore
