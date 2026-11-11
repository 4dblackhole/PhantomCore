#include "System/AudioSystem.h"

#include "Backend/Fmod/FmodAudioBackend.h"

#include <Windows.h>

#include <utility>


namespace PhantomCore
{
std::unique_ptr<IAudioBackend> CreateFmodAudioBackend()
{
    return std::make_unique<FmodAudioBackend>();
}

AudioClip::AudioClip(
    std::unique_ptr<IAudioClipBackend> implementation,
    std::shared_ptr<std::atomic_size_t> liveObjectCount)
    : implementation_(std::move(implementation)),
      liveObjectCount_(std::move(liveObjectCount))
{
    if (liveObjectCount_ != nullptr)
    {
        liveObjectCount_->fetch_add(1, std::memory_order_relaxed);
    }
}

AudioClip::~AudioClip()
{
    // Release the native sound before decrementing the count so mixer
    // reconfiguration cannot start while destruction is still in flight.
    implementation_.reset();
    if (liveObjectCount_ != nullptr)
    {
        liveObjectCount_->fetch_sub(1, std::memory_order_release);
    }
}

bool AudioClip::PrepareTimeStretch(double rate, std::string &error)
{
    if (implementation_) return implementation_->PrepareTimeStretch(rate, error);
    error = "The audio clip is not initialized.";
    return false;
}

bool AudioClip::IsValid() const noexcept
{
    return implementation_ != nullptr;
}

AudioClipLoadState AudioClip::LoadState(std::string& errorMessage) const
{
    if (implementation_)
        return implementation_->LoadState(errorMessage);
    errorMessage = "The audio clip is not initialized.";
    return AudioClipLoadState::Failed;
}

bool AudioClip::GetDurationMicroseconds(std::uint64_t& duration, std::string& errorMessage) const
{
    if (implementation_)
        return implementation_->GetDurationMicroseconds(duration, errorMessage);
    duration = 0;
    errorMessage = "The audio clip is not initialized.";
    return false;
}

bool AudioClip::Play(std::string& errorMessage)
{
    return Play(AudioPlaybackSettings{}, nullptr, errorMessage) != nullptr;
}

std::unique_ptr<AudioVoice> AudioClip::Play(
    const AudioPlaybackSettings& settings,
    AudioBus* const bus,
    std::string& errorMessage)
{
    if (implementation_ == nullptr)
    {
        errorMessage = "The audio clip is not initialized.";
        return nullptr;
    }

    IAudioBusBackend* const busImplementation =
        bus != nullptr ? bus->implementation_.get() : nullptr;
    std::unique_ptr<IAudioVoiceBackend> voice = implementation_->Play(
        settings,
        busImplementation,
        errorMessage);
    if (voice == nullptr)
    {
        return nullptr;
    }
    return std::unique_ptr<AudioVoice>(new AudioVoice(
        std::move(voice),
        liveObjectCount_));
}

AudioSystem::~AudioSystem()
{
    Shutdown();
}

bool AudioSystem::Initialize(
    const AudioConfig& config,
    const AudioBackendFactory backendFactory,
    const AudioClipBackendFactory clipFactory,
    std::string& errorMessage)
{
    Shutdown();

    const AudioBackendFactory selectedBackendFactory =
        backendFactory != nullptr
            ? backendFactory
            : &CreateFmodAudioBackend;
    clipFactory_ = clipFactory != nullptr
        ? clipFactory
        : (backendFactory == nullptr
            ? &CreateFmodAudioClipBackend
            : nullptr);
    liveObjectCount_ = std::make_shared<std::atomic_size_t>(0);

    if (InitializeExactBackend(
            config,
            selectedBackendFactory,
            errorMessage))
    {
        return true;
    }

    std::string combinedError = errorMessage;
    if (config.preferredBackend == AudioOutputBackend::Asio &&
        config.fallBackToWasapi)
    {
        AudioConfig fallback = config;
        fallback.preferredBackend = AudioOutputBackend::Wasapi;
        // Driver indices belong to one output API and cannot be reused
        // when falling back to another API.
        fallback.driverIndex = -1;
        std::string fallbackError;
        if (InitializeExactBackend(
                fallback,
                selectedBackendFactory,
                fallbackError))
        {
            errorMessage.clear();
            return true;
        }
        combinedError += " | WASAPI fallback: " + fallbackError;
    }

    if (config.allowNoSoundFallback)
    {
        AudioConfig fallback = config;
        fallback.preferredBackend = AudioOutputBackend::NoSound;
        fallback.driverIndex = -1;
        std::string fallbackError;
        if (InitializeExactBackend(
                fallback,
                selectedBackendFactory,
                fallbackError))
        {
            errorMessage.clear();
            return true;
        }
        combinedError += " | no-sound fallback: " + fallbackError;
    }

    errorMessage = std::move(combinedError);
    clipFactory_ = nullptr;
    liveObjectCount_.reset();
    return false;
}

void AudioSystem::Update()
{
    if (initialized_ && backend_ != nullptr)
    {
        backend_->Update();
    }
    std::erase_if(retiredClips_, [](const auto& clip) {
        std::string error;
        const auto state = clip->LoadState(error);
        return state == AudioClipLoadState::Ready || state == AudioClipLoadState::Failed;
    });
}

void AudioSystem::RetireClip(std::shared_ptr<AudioClip> clip)
{
    if (clip)
        retiredClips_.push_back(std::move(clip));
}

void AudioSystem::Shutdown() noexcept
{
    // Finish retired native sound I/O while its loader/system is alive.
    // Closing FMOD first can wait on an unfinished async seek indefinitely.
    // Runtime retirement still polls; only final shutdown may wait here.
    retiredClips_.clear();
    if (backend_ != nullptr)
    {
        backend_->Shutdown();
        backend_.reset();
    }
    initialized_ = false;
    clipFactory_ = nullptr;
    config_ = {};
    liveObjectCount_.reset();
}

bool AudioSystem::IsInitialized() const noexcept
{
    return initialized_;
}

std::string_view AudioSystem::BackendName() const noexcept
{
    return backend_ != nullptr ? backend_->Name() : "None";
}

AudioOutputBackend AudioSystem::RequestedOutput() const noexcept
{
    return backend_ != nullptr
        ? backend_->RequestedOutput()
        : AudioOutputBackend::NoSound;
}

AudioOutputBackend AudioSystem::ActiveOutput() const noexcept
{
    return backend_ != nullptr
        ? backend_->ActiveOutput()
        : AudioOutputBackend::NoSound;
}

bool AudioSystem::SetOutputBackend(
    const AudioOutputBackend backend,
    std::string& errorMessage)
{
    if (!initialized_ || backend_ == nullptr)
    {
        errorMessage = "The audio system is not initialized.";
        return false;
    }
    if (!backend_->SetOutputBackend(backend, errorMessage))
    {
        return false;
    }
    config_.preferredBackend = backend;
    config_.driverIndex = backend_->ActiveDriverIndex();
    return true;
}

int AudioSystem::DriverCount() const noexcept
{
    return backend_ != nullptr ? backend_->DriverCount() : 0;
}

const std::vector<AudioDeviceInfo>&
AudioSystem::OutputDrivers() const noexcept
{
    static const std::vector<AudioDeviceInfo> empty;
    return backend_ != nullptr ? backend_->OutputDrivers() : empty;
}

int AudioSystem::ActiveDriverIndex() const noexcept
{
    return backend_ != nullptr ? backend_->ActiveDriverIndex() : -1;
}

bool AudioSystem::SetOutputDriver(
    const int driverIndex,
    std::string& errorMessage)
{
    if (!initialized_ || backend_ == nullptr)
    {
        errorMessage = "The audio system is not initialized.";
        return false;
    }
    if (!backend_->SetOutputDriver(driverIndex, errorMessage))
    {
        return false;
    }
    config_.driverIndex = driverIndex;
    return true;
}

int AudioSystem::RequestedSampleRate() const noexcept
{
    return backend_ != nullptr ? backend_->RequestedSampleRate() : 0;
}

int AudioSystem::SampleRate() const noexcept
{
    return backend_ != nullptr ? backend_->SampleRate() : 0;
}

std::uint32_t AudioSystem::DspBufferLength() const noexcept
{
    return backend_ != nullptr ? backend_->DspBufferLength() : 0;
}

int AudioSystem::DspBufferCount() const noexcept
{
    return backend_ != nullptr ? backend_->DspBufferCount() : 0;
}

double AudioSystem::EstimatedDspLatencyMilliseconds() const noexcept
{
    return backend_ != nullptr
        ? backend_->EstimatedDspLatencyMilliseconds()
        : 0.0;
}

bool AudioSystem::SetSampleRate(
    const int sampleRate,
    std::string& errorMessage)
{
    if (!CanRestartMixer(errorMessage))
    {
        return false;
    }
    if (!backend_->SetSampleRate(sampleRate, errorMessage))
    {
        return false;
    }
    config_.sampleRate = sampleRate;
    return true;
}

bool AudioSystem::SetDspBufferSize(
    const std::uint32_t bufferLength,
    const int bufferCount,
    std::string& errorMessage)
{
    if (!CanRestartMixer(errorMessage))
    {
        return false;
    }
    if (!backend_->SetDspBufferSize(
            bufferLength,
            bufferCount,
            errorMessage))
    {
        return false;
    }
    config_.dspBufferLength = bufferLength;
    config_.dspBufferCount = bufferCount;
    return true;
}

std::uint64_t AudioSystem::DspClock() const noexcept
{
    return backend_ != nullptr ? backend_->DspClock() : 0;
}

AudioClockSnapshot AudioSystem::CaptureClockSnapshot() const noexcept
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return AudioClockSnapshot{
        DspClock(),
        SampleRate(),
        counter.QuadPart,
        frequency.QuadPart};
}

std::unique_ptr<AudioClip> AudioSystem::LoadSound(
    const std::filesystem::path& path,
    const AudioLoadMode loadMode,
    std::string& errorMessage)
{
    if (!initialized_ || backend_ == nullptr)
    {
        errorMessage = "The audio system is not initialized.";
        return nullptr;
    }
    if (clipFactory_ == nullptr)
    {
        errorMessage = "The selected audio backend has no clip factory.";
        return nullptr;
    }

    std::unique_ptr<IAudioClipBackend> implementation =
        clipFactory_(*backend_, path, loadMode, errorMessage);
    if (implementation == nullptr)
    {
        return nullptr;
    }
    return std::unique_ptr<AudioClip>(new AudioClip(
        std::move(implementation),
        liveObjectCount_));
}

std::unique_ptr<AudioClip> AudioSystem::LoadSound(
    const std::filesystem::path& path,
    std::string& errorMessage)
{
    return LoadSound(path, AudioLoadMode::Sample, errorMessage);
}

std::unique_ptr<AudioBus> AudioSystem::CreateBus(
    const std::string_view name,
    AudioBus* const parent,
    std::string& errorMessage)
{
    if (!initialized_ || backend_ == nullptr)
    {
        errorMessage = "The audio system is not initialized.";
        return nullptr;
    }

    IAudioBusBackend* const parentImplementation =
        parent != nullptr ? parent->implementation_.get() : nullptr;
    std::unique_ptr<IAudioBusBackend> implementation =
        backend_->CreateBus(name, parentImplementation, errorMessage);
    if (implementation == nullptr)
    {
        return nullptr;
    }
    return std::unique_ptr<AudioBus>(new AudioBus(
        std::move(implementation),
        liveObjectCount_));
}

bool AudioSystem::InitializeExactBackend(
    const AudioConfig& config,
    const AudioBackendFactory factory,
    std::string& errorMessage)
{
    if (backend_ != nullptr)
    {
        backend_->Shutdown();
        backend_.reset();
    }

    backend_ = factory != nullptr ? factory() : nullptr;
    if (backend_ == nullptr)
    {
        errorMessage = "The audio backend factory returned null.";
        initialized_ = false;
        return false;
    }

    initialized_ = backend_->Initialize(config, errorMessage);
    if (!initialized_)
    {
        backend_->Shutdown();
        backend_.reset();
        return false;
    }
    config_ = config;
    return true;
}

bool AudioSystem::CanRestartMixer(std::string& errorMessage) const
{
    if (!initialized_ || backend_ == nullptr)
    {
        errorMessage = "The audio system is not initialized.";
        return false;
    }
    if (liveObjectCount_ != nullptr &&
        liveObjectCount_->load(std::memory_order_acquire) != 0)
    {
        errorMessage =
            "Release all Client-owned audio clips, voices, buses, and "
            "effects before changing "
            "the sample rate or DSP buffer size.";
        return false;
    }
    return true;
}

} // namespace PhantomCore
