#pragma once

#include "System/AudioSystem.h"

#include <fmod.hpp>

#include <cstdint>
#include <memory>


// Shared only with Client-owned FmodAudioClip objects so they can detect
// that their native FMOD objects were invalidated by system shutdown.
namespace PhantomCore
{
struct FmodSystemLifetime
{
    FMOD::System* system{};
    std::uint64_t generation{};
};

class FmodAudioBackend final : public IAudioBackend
{
public:
    ~FmodAudioBackend() override;

    [[nodiscard]] bool Initialize(
        const AudioConfig& config,
        std::string& errorMessage) override;
    void Update() override;
    void Shutdown() noexcept override;

    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] AudioOutputBackend RequestedOutput() const noexcept override;
    [[nodiscard]] AudioOutputBackend ActiveOutput() const noexcept override;
    [[nodiscard]] bool SetOutputBackend(
        AudioOutputBackend backend,
        std::string& errorMessage) override;

    [[nodiscard]] int DriverCount() const noexcept override;
    [[nodiscard]] const std::vector<AudioDeviceInfo>&
        OutputDrivers() const noexcept override;
    [[nodiscard]] int ActiveDriverIndex() const noexcept override;
    [[nodiscard]] bool SetOutputDriver(
        int driverIndex,
        std::string& errorMessage) override;

    [[nodiscard]] int RequestedSampleRate() const noexcept override;
    [[nodiscard]] int SampleRate() const noexcept override;
    [[nodiscard]] std::uint32_t DspBufferLength() const noexcept override;
    [[nodiscard]] int DspBufferCount() const noexcept override;
    [[nodiscard]] double EstimatedDspLatencyMilliseconds() const noexcept override;
    [[nodiscard]] bool SetSampleRate(
        int sampleRate,
        std::string& errorMessage) override;
    [[nodiscard]] bool SetDspBufferSize(
        std::uint32_t bufferLength,
        int bufferCount,
        std::string& errorMessage) override;

    [[nodiscard]] std::uint64_t DspClock() const noexcept override;
    [[nodiscard]] std::unique_ptr<IAudioBusBackend> CreateBus(
        std::string_view name,
        IAudioBusBackend* parent,
        std::string& errorMessage) override;

    // Internal FMOD feature accessors. These are not selected for the
    // generated Client SDK and avoid friend-based coupling between the
    // backend and its clip/bus implementations.
    [[nodiscard]] FMOD::System* NativeSystem() const noexcept;
    [[nodiscard]] const std::shared_ptr<FmodSystemLifetime>&
        Lifetime() const noexcept;
    [[nodiscard]] bool IsMixerInitialized() const noexcept;

private:
    [[nodiscard]] bool CreateSystem(std::string& errorMessage);
    [[nodiscard]] bool InitializeMixer(
        AudioOutputBackend output,
        int sampleRate,
        std::uint32_t bufferLength,
        int bufferCount,
        int driverIndex,
        std::string& errorMessage);
    [[nodiscard]] bool RestartMixer(
        int sampleRate,
        std::uint32_t bufferLength,
        int bufferCount,
        std::string& errorMessage);
    void RestoreOutputAfterFailedSwitch(
        AudioOutputBackend previousRequestedOutput,
        const std::string& switchError,
        std::string& errorMessage);
    [[nodiscard]] bool EnumerateCurrentDrivers(std::string& errorMessage);
    void RefreshRuntimeState() noexcept;
    void InvalidateNativeObjects() noexcept;

    FMOD::System* system_{};
    FMOD::ChannelGroup* masterChannelGroup_{};
    std::shared_ptr<FmodSystemLifetime> lifetime_;
    AudioOutputBackend requestedOutput_{AudioOutputBackend::Automatic};
    AudioOutputBackend activeOutput_{AudioOutputBackend::NoSound};
    int driverCount_{};
    int activeDriverIndex_{-1};
    std::vector<AudioDeviceInfo> currentDrivers_;
    int requestedSampleRate_{};
    int sampleRate_{};
    std::uint32_t dspBufferLength_{};
    int dspBufferCount_{};
    int maxVirtualChannels_{256};
    void* nativeWindowHandle_{};
    bool initialized_{};
};

} // namespace PhantomCore
