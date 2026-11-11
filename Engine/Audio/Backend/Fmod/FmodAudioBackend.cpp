#include "FmodAudioBackend.h"
#include "Backend/Fmod/FmodError.h"

#include "FmodAudioBus.h"

#include <fmod_errors.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>


namespace PhantomCore
{
namespace
{
    FMOD_OUTPUTTYPE ToFmodOutput(const AudioOutputBackend backend) noexcept
    {
        switch (backend)
        {
        case AudioOutputBackend::Wasapi:
            return FMOD_OUTPUTTYPE_WASAPI;
        case AudioOutputBackend::Asio:
            return FMOD_OUTPUTTYPE_ASIO;
        case AudioOutputBackend::NoSound:
            return FMOD_OUTPUTTYPE_NOSOUND;
        case AudioOutputBackend::Automatic:
        default:
            return FMOD_OUTPUTTYPE_AUTODETECT;
        }
    }

    AudioOutputBackend FromFmodOutput(const FMOD_OUTPUTTYPE output) noexcept
    {
        switch (output)
        {
        case FMOD_OUTPUTTYPE_WASAPI:
            return AudioOutputBackend::Wasapi;
        case FMOD_OUTPUTTYPE_ASIO:
            return AudioOutputBackend::Asio;
        case FMOD_OUTPUTTYPE_NOSOUND:
            return AudioOutputBackend::NoSound;
        default:
            return AudioOutputBackend::Automatic;
        }
    }

    const char* OutputBackendName(
        const AudioOutputBackend backend) noexcept
    {
        switch (backend)
        {
        case AudioOutputBackend::Wasapi:
            return "WASAPI";
        case AudioOutputBackend::Asio:
            return "ASIO";
        case AudioOutputBackend::NoSound:
            return "NoSound";
        case AudioOutputBackend::Automatic:
        default:
            return "Automatic";
        }
    }

    bool OutputMatchesRequest(
        const AudioOutputBackend requested,
        const AudioOutputBackend active) noexcept
    {
        // Autodetect intentionally resolves to a concrete output API.
        return requested == AudioOutputBackend::Automatic ||
            requested == active;
    }

    std::string MakeOutputMismatchError(
        const AudioOutputBackend requested,
        const AudioOutputBackend active)
    {
        return std::string("FMOD requested ") +
            OutputBackendName(requested) + " but activated " +
            OutputBackendName(active) + ".";
    }

    void DebugLog(const std::string& message)
    {
        OutputDebugStringA(("[PhantomCore.Audio] " + message + "\n").c_str());
    }
}

FmodAudioBackend::~FmodAudioBackend()
{
    Shutdown();
}

bool FmodAudioBackend::Initialize(
    const AudioConfig& config,
    std::string& errorMessage)
{
    Shutdown();
    if (config.sampleRate != 0 &&
        (config.sampleRate < 8000 || config.sampleRate > 192000))
    {
        errorMessage = "The FMOD mixer sample rate must be between 8000 and 192000 Hz.";
        return false;
    }
    if (config.dspBufferLength == 0 || config.dspBufferCount <= 0)
    {
        errorMessage = "The FMOD DSP buffer length and count must be positive.";
        return false;
    }

    requestedOutput_ = config.preferredBackend;
    requestedSampleRate_ = config.sampleRate;
    dspBufferLength_ = config.dspBufferLength;
    dspBufferCount_ = config.dspBufferCount;
    maxVirtualChannels_ = config.maxVirtualChannels;
    nativeWindowHandle_ = config.nativeWindowHandle;

    if (!CreateSystem(errorMessage) ||
        !InitializeMixer(
            requestedOutput_,
            requestedSampleRate_,
            dspBufferLength_,
            dspBufferCount_,
            -1,
            errorMessage))
    {
        Shutdown();
        return false;
    }

    // The first driver query occurs only after System::init succeeds and
    // is scoped to the one output API that FMOD actually selected.
    if (!EnumerateCurrentDrivers(errorMessage))
    {
        Shutdown();
        return false;
    }
    if (config.driverIndex >= 0 &&
        !SetOutputDriver(config.driverIndex, errorMessage))
    {
        Shutdown();
        return false;
    }

    errorMessage.clear();
    return true;
}

void FmodAudioBackend::Update()
{
    if (!initialized_ || system_ == nullptr)
    {
        return;
    }
    const FMOD_RESULT result = system_->update();
    if (result != FMOD_OK)
    {
        DebugLog(FmodDetail::MakeFmodError("FMOD::System::update", result));
    }
}

void FmodAudioBackend::Shutdown() noexcept
{
    InvalidateNativeObjects();
    masterChannelGroup_ = nullptr;
    if (system_ != nullptr)
    {
        if (initialized_)
        {
            static_cast<void>(system_->close());
        }
        static_cast<void>(system_->release());
        system_ = nullptr;
    }

    lifetime_.reset();
    requestedOutput_ = AudioOutputBackend::Automatic;
    activeOutput_ = AudioOutputBackend::NoSound;
    driverCount_ = 0;
    activeDriverIndex_ = -1;
    currentDrivers_.clear();
    requestedSampleRate_ = 0;
    sampleRate_ = 0;
    dspBufferLength_ = 0;
    dspBufferCount_ = 0;
    maxVirtualChannels_ = 256;
    nativeWindowHandle_ = nullptr;
    initialized_ = false;
}

std::string_view FmodAudioBackend::Name() const noexcept
{
    return "FMOD Core";
}

AudioOutputBackend FmodAudioBackend::RequestedOutput() const noexcept
{
    return requestedOutput_;
}

AudioOutputBackend FmodAudioBackend::ActiveOutput() const noexcept
{
    return activeOutput_;
}

bool FmodAudioBackend::SetOutputBackend(
    const AudioOutputBackend backend,
    std::string& errorMessage)
{
    if (!initialized_ || system_ == nullptr)
    {
        errorMessage = "FMOD is not initialized.";
        return false;
    }
    if (backend == requestedOutput_)
    {
        errorMessage.clear();
        return true;
    }

    const AudioOutputBackend previousRequestedOutput = requestedOutput_;
    const FMOD_RESULT result = system_->setOutput(ToFmodOutput(backend));
    if (result != FMOD_OK)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::System::setOutput", result);
        return false;
    }

    RefreshRuntimeState();
    if (!OutputMatchesRequest(backend, activeOutput_))
    {
        RestoreOutputAfterFailedSwitch(
            previousRequestedOutput,
            MakeOutputMismatchError(backend, activeOutput_),
            errorMessage);
        return false;
    }

    requestedOutput_ = backend;
    if (!EnumerateCurrentDrivers(errorMessage))
    {
        return false;
    }
    errorMessage.clear();
    return true;
}

int FmodAudioBackend::DriverCount() const noexcept
{
    return driverCount_;
}

const std::vector<AudioDeviceInfo>&
FmodAudioBackend::OutputDrivers() const noexcept
{
    return currentDrivers_;
}

int FmodAudioBackend::ActiveDriverIndex() const noexcept
{
    return activeDriverIndex_;
}

bool FmodAudioBackend::SetOutputDriver(
    const int driverIndex,
    std::string& errorMessage)
{
    if (!initialized_ || system_ == nullptr)
    {
        errorMessage = "FMOD is not initialized.";
        return false;
    }
    if (driverIndex < 0 || driverIndex >= driverCount_)
    {
        errorMessage = "The selected FMOD output driver index is invalid.";
        return false;
    }
    if (driverIndex == activeDriverIndex_)
    {
        errorMessage.clear();
        return true;
    }

    const FMOD_RESULT result = system_->setDriver(driverIndex);
    if (result != FMOD_OK)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::System::setDriver", result);
        return false;
    }
    RefreshRuntimeState();
    errorMessage.clear();
    return true;
}

int FmodAudioBackend::RequestedSampleRate() const noexcept
{
    return requestedSampleRate_;
}

int FmodAudioBackend::SampleRate() const noexcept
{
    return sampleRate_;
}

std::uint32_t FmodAudioBackend::DspBufferLength() const noexcept
{
    return dspBufferLength_;
}

int FmodAudioBackend::DspBufferCount() const noexcept
{
    return dspBufferCount_;
}

double FmodAudioBackend::EstimatedDspLatencyMilliseconds() const noexcept
{
    if (sampleRate_ <= 0 || dspBufferLength_ == 0 || dspBufferCount_ <= 0)
    {
        return 0.0;
    }
    const double audibleBufferCount = std::max(
        0.5,
        static_cast<double>(dspBufferCount_) - 1.5);
    return static_cast<double>(dspBufferLength_) *
        audibleBufferCount * 1000.0 /
        static_cast<double>(sampleRate_);
}

bool FmodAudioBackend::SetSampleRate(
    const int sampleRate,
    std::string& errorMessage)
{
    if (sampleRate < 8000 || sampleRate > 192000)
    {
        errorMessage = "The FMOD mixer sample rate must be between 8000 and 192000 Hz.";
        return false;
    }
    if (sampleRate == requestedSampleRate_)
    {
        errorMessage.clear();
        return true;
    }
    return RestartMixer(
        sampleRate,
        dspBufferLength_,
        dspBufferCount_,
        errorMessage);
}

bool FmodAudioBackend::SetDspBufferSize(
    const std::uint32_t bufferLength,
    const int bufferCount,
    std::string& errorMessage)
{
    if (bufferLength == 0 || bufferCount <= 0)
    {
        errorMessage = "The FMOD DSP buffer length and count must be positive.";
        return false;
    }
    if (bufferLength == dspBufferLength_ &&
        bufferCount == dspBufferCount_)
    {
        errorMessage.clear();
        return true;
    }
    return RestartMixer(
        requestedSampleRate_,
        bufferLength,
        bufferCount,
        errorMessage);
}

std::uint64_t FmodAudioBackend::DspClock() const noexcept
{
    if (masterChannelGroup_ == nullptr)
    {
        return 0;
    }

    unsigned long long dspClock = 0;
    unsigned long long parentClock = 0;
    if (masterChannelGroup_->getDSPClock(
            &dspClock,
            &parentClock) != FMOD_OK)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(dspClock);
}

std::unique_ptr<IAudioBusBackend> FmodAudioBackend::CreateBus(
    const std::string_view name,
    IAudioBusBackend* const parent,
    std::string& errorMessage)
{
    if (!initialized_ || system_ == nullptr || masterChannelGroup_ == nullptr)
    {
        errorMessage = "FMOD is not initialized.";
        return nullptr;
    }
    if (name.empty())
    {
        errorMessage = "An audio bus name cannot be empty.";
        return nullptr;
    }

    FMOD::ChannelGroup* parentGroup = masterChannelGroup_;
    if (parent != nullptr)
    {
        auto* const fmodParent = dynamic_cast<FmodAudioBus*>(parent);
        if (fmodParent == nullptr || !fmodParent->HasLiveSystem())
        {
            errorMessage = "The parent audio bus belongs to another backend or is invalid.";
            return nullptr;
        }
        parentGroup = fmodParent->NativeGroup();
    }

    const std::string ownedName(name);
    FMOD::ChannelGroup* group = nullptr;
    FMOD_RESULT result = system_->createChannelGroup(
        ownedName.c_str(),
        &group);
    if (result != FMOD_OK || group == nullptr)
    {
        errorMessage = FmodDetail::MakeFmodError(
            "FMOD::System::createChannelGroup",
            result);
        return nullptr;
    }

    result = parentGroup->addGroup(group);
    if (result != FMOD_OK)
    {
        static_cast<void>(group->release());
        errorMessage = FmodDetail::MakeFmodError(
            "FMOD::ChannelGroup::addGroup",
            result);
        return nullptr;
    }

    errorMessage.clear();
    return std::make_unique<FmodAudioBus>(
        lifetime_,
        group,
        ownedName);
}

FMOD::System* FmodAudioBackend::NativeSystem() const noexcept
{
    return initialized_ ? system_ : nullptr;
}

const std::shared_ptr<FmodSystemLifetime>&
FmodAudioBackend::Lifetime() const noexcept
{
    return lifetime_;
}

bool FmodAudioBackend::IsMixerInitialized() const noexcept
{
    return initialized_ && system_ != nullptr;
}

bool FmodAudioBackend::CreateSystem(std::string& errorMessage)
{
    FMOD_RESULT result = FMOD::System_Create(&system_);
    if (result != FMOD_OK || system_ == nullptr)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::System_Create", result);
        system_ = nullptr;
        return false;
    }

    unsigned int runtimeVersion = 0;
    result = system_->getVersion(&runtimeVersion);
    if (result != FMOD_OK || runtimeVersion < FMOD_VERSION)
    {
        errorMessage = result != FMOD_OK
            ? FmodDetail::MakeFmodError("FMOD::System::getVersion", result)
            : "The FMOD runtime DLL is older than the FMOD headers.";
        return false;
    }

    lifetime_ = std::make_shared<FmodSystemLifetime>();
    errorMessage.clear();
    return true;
}

bool FmodAudioBackend::InitializeMixer(
    const AudioOutputBackend output,
    const int sampleRate,
    const std::uint32_t bufferLength,
    const int bufferCount,
    const int driverIndex,
    std::string& errorMessage)
{
    FMOD_RESULT result = system_->setOutput(ToFmodOutput(output));
    if (result != FMOD_OK)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::System::setOutput", result);
        return false;
    }

    if (sampleRate > 0)
    {
        result = system_->setSoftwareFormat(
            sampleRate,
            FMOD_SPEAKERMODE_DEFAULT,
            0);
        if (result != FMOD_OK)
        {
            errorMessage = FmodDetail::MakeFmodError(
                "FMOD::System::setSoftwareFormat",
                result);
            return false;
        }
    }

    result = system_->setDSPBufferSize(bufferLength, bufferCount);
    if (result != FMOD_OK)
    {
        errorMessage = FmodDetail::MakeFmodError(
            "FMOD::System::setDSPBufferSize",
            result);
        return false;
    }

    if (driverIndex >= 0 && output != AudioOutputBackend::NoSound)
    {
        result = system_->setDriver(driverIndex);
        if (result != FMOD_OK)
        {
            errorMessage = FmodDetail::MakeFmodError("FMOD::System::setDriver", result);
            return false;
        }
    }

    // The HWND is optional for other Windows outputs and is retained so a
    // later live switch to ASIO has the application handle FMOD expects.
    result = system_->init(
        maxVirtualChannels_,
        FMOD_INIT_NORMAL,
        nativeWindowHandle_);
    if (result != FMOD_OK)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::System::init", result);
        return false;
    }

    initialized_ = true;
    if (lifetime_ != nullptr)
    {
        lifetime_->system = system_;
    }
    RefreshRuntimeState();
    if (!OutputMatchesRequest(output, activeOutput_))
    {
        errorMessage = MakeOutputMismatchError(output, activeOutput_);

        // System::init succeeded but resolved an exact request to another
        // output. Close that mixer so callers can retry a fallback safely.
        InvalidateNativeObjects();
        masterChannelGroup_ = nullptr;
        const FMOD_RESULT closeResult = system_->close();
        initialized_ = false;
        if (closeResult != FMOD_OK)
        {
            errorMessage += " " +
                FmodDetail::MakeFmodError("FMOD::System::close", closeResult);
        }
        return false;
    }
    errorMessage.clear();
    return true;
}

void FmodAudioBackend::RestoreOutputAfterFailedSwitch(
    const AudioOutputBackend previousRequestedOutput,
    const std::string& switchError,
    std::string& errorMessage)
{
    // A failed exact switch can leave FMOD on NoSound. Restore the last
    // requested output so the running game keeps usable audio.
    const FMOD_RESULT restoreResult = system_->setOutput(
        ToFmodOutput(previousRequestedOutput));
    RefreshRuntimeState();
    requestedOutput_ = previousRequestedOutput;

    std::string enumerationError;
    const bool enumerated = EnumerateCurrentDrivers(enumerationError);
    if (restoreResult != FMOD_OK)
    {
        errorMessage = switchError + " Restore failed: " +
            FmodDetail::MakeFmodError("FMOD::System::setOutput", restoreResult);
    }
    else if (!enumerated)
    {
        errorMessage = switchError +
            " The previous output was restored, but driver enumeration failed: " +
            enumerationError;
    }
    else
    {
        errorMessage = switchError + " The previous output was restored.";
    }
}

bool FmodAudioBackend::RestartMixer(
    const int sampleRate,
    const std::uint32_t bufferLength,
    const int bufferCount,
    std::string& errorMessage)
{
    if (!initialized_ || system_ == nullptr)
    {
        errorMessage = "FMOD is not initialized.";
        return false;
    }

    const int previousSampleRate = requestedSampleRate_;
    const int previousActualSampleRate = sampleRate_;
    const std::uint32_t previousBufferLength = dspBufferLength_;
    const int previousBufferCount = dspBufferCount_;
    const int previousDriver = activeDriverIndex_;
    const AudioOutputBackend previousActiveOutput = activeOutput_;

    InvalidateNativeObjects();
    masterChannelGroup_ = nullptr;
    const FMOD_RESULT closeResult = system_->close();
    initialized_ = false;
    if (closeResult != FMOD_OK)
    {
        errorMessage = FmodDetail::MakeFmodError("FMOD::System::close", closeResult);
        return false;
    }

    std::string requestedError;
    if (InitializeMixer(
            requestedOutput_,
            sampleRate,
            bufferLength,
            bufferCount,
            previousDriver,
            requestedError))
    {
        requestedSampleRate_ = sampleRate;
        if (activeOutput_ != previousActiveOutput &&
            !EnumerateCurrentDrivers(errorMessage))
        {
            return false;
        }
        errorMessage.clear();
        return true;
    }

    // Restore the last working configuration when the new mixer values
    // are rejected. No clips can be alive while AudioSystem calls here.
    std::string recoveryError;
    if (InitializeMixer(
            requestedOutput_,
            previousSampleRate > 0
                ? previousSampleRate
                : previousActualSampleRate,
            previousBufferLength,
            previousBufferCount,
            previousDriver,
            recoveryError))
    {
        requestedSampleRate_ = previousSampleRate;
        errorMessage = requestedError + " Previous mixer settings were restored.";
        return false;
    }

    errorMessage = requestedError + " | Recovery failed: " + recoveryError;
    Shutdown();
    return false;
}

bool FmodAudioBackend::EnumerateCurrentDrivers(
    std::string& errorMessage)
{
    currentDrivers_.clear();
    driverCount_ = 0;
    if (activeOutput_ == AudioOutputBackend::NoSound)
    {
        errorMessage.clear();
        return true;
    }

    FMOD_RESULT result = system_->getNumDrivers(&driverCount_);
    if (result != FMOD_OK)
    {
        errorMessage = FmodDetail::MakeFmodError(
            "FMOD::System::getNumDrivers",
            result);
        driverCount_ = 0;
        return false;
    }

    currentDrivers_.reserve(static_cast<std::size_t>(driverCount_));
    for (int driverIndex = 0; driverIndex < driverCount_; ++driverIndex)
    {
        std::array<char, 512> name{};
        FMOD_GUID guid{};
        int systemRate = 0;
        FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_DEFAULT;
        int speakerChannels = 0;
        result = system_->getDriverInfo(
            driverIndex,
            name.data(),
            static_cast<int>(name.size()),
            &guid,
            &systemRate,
            &speakerMode,
            &speakerChannels);
        if (result != FMOD_OK)
        {
            DebugLog(
                FmodDetail::MakeFmodError("FMOD::System::getDriverInfo", result) +
                " for " + OutputBackendName(activeOutput_) +
                " driver " + std::to_string(driverIndex));
            continue;
        }

        currentDrivers_.push_back(AudioDeviceInfo{
            activeOutput_,
            driverIndex,
            name.data(),
            systemRate,
            speakerChannels});
    }

    DebugLog(
        std::string(OutputBackendName(activeOutput_)) +
        " enumeration found " +
        std::to_string(currentDrivers_.size()) + " of " +
        std::to_string(driverCount_) + " FMOD drivers.");
    errorMessage.clear();
    return true;
}

void FmodAudioBackend::RefreshRuntimeState() noexcept
{
    FMOD_OUTPUTTYPE output = FMOD_OUTPUTTYPE_UNKNOWN;
    activeOutput_ = system_->getOutput(&output) == FMOD_OK
        ? FromFmodOutput(output)
        : requestedOutput_;

    if (system_->getDriver(&activeDriverIndex_) != FMOD_OK)
    {
        activeDriverIndex_ = -1;
    }

    FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_DEFAULT;
    int rawSpeakers = 0;
    if (system_->getSoftwareFormat(
            &sampleRate_,
            &speakerMode,
            &rawSpeakers) != FMOD_OK)
    {
        sampleRate_ = 0;
    }

    unsigned int bufferLength = 0;
    int bufferCount = 0;
    if (system_->getDSPBufferSize(
            &bufferLength,
            &bufferCount) == FMOD_OK)
    {
        dspBufferLength_ = bufferLength;
        dspBufferCount_ = bufferCount;
    }

    if (system_->getMasterChannelGroup(&masterChannelGroup_) != FMOD_OK)
    {
        masterChannelGroup_ = nullptr;
    }
}

void FmodAudioBackend::InvalidateNativeObjects() noexcept
{
    if (lifetime_ != nullptr)
    {
        lifetime_->system = nullptr;
        ++lifetime_->generation;
    }
}

} // namespace PhantomCore
