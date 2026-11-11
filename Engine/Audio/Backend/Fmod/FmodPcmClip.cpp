#include "FmodPcmClip.h"
#include "FmodAudioClip.h"
#include "Decode/AudioPcmDecoder.h"
#include "Processing/TimeStretch.h"
#include <cmath>
#include <algorithm>

#include <fmod_errors.h>
#include <chrono>
#include <future>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace PhantomCore
{
namespace
{
    struct SourcePcm
    {
        std::uint32_t channels{}, rate{};
        std::vector<float> samples;
    };

    SourcePcm ReadSource(const std::filesystem::path& path, const std::stop_token stop)
    {
        SourcePcm pcm;
        DecodeAudioPcm(path, stop, [&](const std::uint32_t channels, const std::uint32_t rate,
                                       const std::span<const float> samples)
        {
            if (channels == 0 || channels > 32 || rate == 0 ||
                rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                samples.size() % channels != 0 ||
                (pcm.rate != 0 && (pcm.channels != channels || pcm.rate != rate)))
                throw std::runtime_error("PCM source has an invalid or changing format.");
            pcm.channels = channels;
            pcm.rate = rate;
            constexpr auto maxSamples = std::numeric_limits<unsigned int>::max() / sizeof(float);
            if (samples.size() > maxSamples - pcm.samples.size())
                throw std::runtime_error("PCM source exceeds the backend's byte-length range.");
            pcm.samples.insert(pcm.samples.end(), samples.begin(), samples.end());
        });
        if (stop.stop_requested())
            throw std::runtime_error("PCM source loading was cancelled.");
        if (pcm.samples.empty())
            throw std::runtime_error("PCM source contains no audio frames.");
        return pcm;
    }

    bool IsAsync(const AudioLoadMode mode)
    {
        return mode == AudioLoadMode::StreamAsync || mode == AudioLoadMode::StreamAsyncAccurate ||
               mode == AudioLoadMode::SampleAsync;
    }

    class StretchedVoice final : public IAudioVoiceBackend
    {
    public:
        StretchedVoice(std::unique_ptr<IAudioVoiceBackend> voice, double rate, std::uint64_t duration, std::uint64_t physicalDuration)
            : voice_(std::move(voice)), rate_(rate), duration_(duration), physicalDuration_(physicalDuration) {}
        AudioPreparationState PreparationState(std::string &e) override { return voice_->PreparationState(e); }
        bool IsPlaying() const noexcept override { return voice_->IsPlaying(); }
        bool Restart(const AudioPlaybackSettings &settings, IAudioBusBackend *bus, std::string &error) override
        {
            if (settings.startPositionMicroseconds >= duration_) { error = "Source position is past the clip end."; return false; }
            auto mapped = settings;
            mapped.startPositionMicroseconds = std::min(static_cast<std::uint64_t>(settings.startPositionMicroseconds / rate_), physicalDuration_ - 1);
            return voice_->Restart(mapped, bus, error);
        }
        bool Stop(std::string &e) override { return voice_->Stop(e); }
        bool SetPaused(bool v, std::string &e) override { return voice_->SetPaused(v, e); }
        bool SetVolume(float v, std::string &e) override { return voice_->SetVolume(v, e); }
        bool SetPitch(float v, std::string &e) override { return voice_->SetPitch(v, e); }
        bool SetDelay(std::uint64_t a, std::uint64_t b, std::string &e) override { return voice_->SetDelay(a, b, e); }
    private:
        std::unique_ptr<IAudioVoiceBackend> voice_;
        double rate_;
        std::uint64_t duration_, physicalDuration_;
    };

    // The public clip retains the decoded source for raw-memory streams.
    // FMOD owns only playback/mixing; it never parses this compressed file.
    class FmodPcmClip final : public IAudioClipBackend
    {
    public:
        FmodPcmClip(std::shared_ptr<FmodSystemLifetime> lifetime,
                    const std::filesystem::path& path, const AudioLoadMode mode)
            : lifetime_(std::move(lifetime)), generation_(lifetime_->generation), mode_(mode),
              pending_(std::async(IsAsync(mode) ? std::launch::async : std::launch::deferred,
                                  [path, stop = cancellation_.get_token()] { return ReadSource(path, stop); }))
        {
        }
        ~FmodPcmClip() override { cancellation_.request_stop(); }

        bool PrepareTimeStretch(double rate, std::string &error) override
        {
            if (!std::isfinite(rate) || rate < 0.5 || rate > 2.0)
            { error = "Time-stretch rate must be 0.5..2.0."; return false; }
            if (clip_ || converting_ || !failure_.empty())
            { error = "Time stretch must be configured before polling the clip."; return false; }
            stretchRate_ = rate;
            error.clear();
            return true;
        }

        AudioClipLoadState LoadState(std::string& error) const override
        {
            if (!failure_.empty())
            {
                error = failure_;
                return AudioClipLoadState::Failed;
            }
            if (!lifetime_ || !lifetime_->system || lifetime_->generation != generation_)
            {
                error = "The PCM clip no longer belongs to a live audio system.";
                return AudioClipLoadState::Failed;
            }
            if (!clip_)
            {
                const auto state = pending_.wait_for(std::chrono::seconds{0});
                if (state == std::future_status::timeout)
                {
                    error.clear();
                    return AudioClipLoadState::Loading;
                }
                try
                {
                    pcm_ = pending_.get();
                    if (!converting_)
                    {
                        sourceDuration_ = static_cast<std::uint64_t>(std::llround(
                            static_cast<long double>(pcm_.samples.size() / pcm_.channels) * 1'000'000 / pcm_.rate));
                        if (stretchRate_ != 1.0)
                        {
                            converting_ = true;
                            pending_ = std::async(std::launch::async,
                                [pcm = std::move(pcm_), rate = stretchRate_, stop = cancellation_.get_token()]() mutable {
                                    pcm.samples = TimeStretchPcm(pcm.samples, pcm.channels, pcm.rate, rate, stop);
                                    return pcm;
                                });
                            error.clear();
                            return AudioClipLoadState::Loading;
                        }
                    }
                    OpenPcm();
                }
                catch (const std::exception& exception)
                {
                    failure_ = exception.what();
                    error = failure_;
                    return AudioClipLoadState::Failed;
                }
            }
            return clip_->LoadState(error);
        }

        bool GetDurationMicroseconds(std::uint64_t& duration, std::string& error) const override
        {
            duration = 0;
            const auto state = LoadState(error);
            if (state == AudioClipLoadState::Loading)
            {
                error = "The PCM source is still loading.";
                return false;
            }
            if (state == AudioClipLoadState::Failed) return false;
            duration = sourceDuration_;
            error.clear();
            return true;
        }

        std::unique_ptr<IAudioVoiceBackend> Play(const AudioPlaybackSettings& settings,
                                                IAudioBusBackend* bus, std::string& error) override
        {
            const auto state = LoadState(error);
            if (state == AudioClipLoadState::Loading)
                error = "The PCM source is still loading.";
            if (state != AudioClipLoadState::Ready && state != AudioClipLoadState::Playing)
                return nullptr;
            if (settings.startPositionMicroseconds >= sourceDuration_)
            { error = "Source position is past the clip end."; return nullptr; }
            std::uint64_t physicalDuration{};
            if (!clip_->GetDurationMicroseconds(physicalDuration, error) || physicalDuration == 0) return nullptr;
            auto mapped = settings;
            mapped.startPositionMicroseconds = std::min(static_cast<std::uint64_t>(settings.startPositionMicroseconds / stretchRate_), physicalDuration - 1);
            auto voice = clip_->Play(mapped, bus, error);
            if (!voice || stretchRate_ == 1.0) return voice;
            return std::make_unique<StretchedVoice>(std::move(voice), stretchRate_, sourceDuration_, physicalDuration);
        }

    private:
        void OpenPcm() const
        {
            FMOD_CREATESOUNDEXINFO info{};
            info.cbsize = sizeof(info);
            info.length = static_cast<unsigned int>(pcm_.samples.size() * sizeof(float));
            info.numchannels = static_cast<int>(pcm_.channels);
            info.defaultfrequency = static_cast<int>(pcm_.rate);
            info.format = FMOD_SOUND_FORMAT_PCMFLOAT;
            const bool sample = mode_ == AudioLoadMode::Sample || mode_ == AudioLoadMode::SampleAsync;
            const FMOD_MODE flags = FMOD_OPENMEMORY | FMOD_OPENRAW |
                                    (sample ? FMOD_CREATESAMPLE : FMOD_CREATESTREAM) |
                                    (IsAsync(mode_) ? FMOD_NONBLOCKING : 0);
            FMOD::Sound* sound{};
            const auto result = lifetime_->system->createSound(
                reinterpret_cast<const char*>(pcm_.samples.data()), flags, &info, &sound);
            if (result != FMOD_OK || !sound)
                throw std::runtime_error(std::string("FMOD PCM source creation failed: ") +
                                         FMOD_ErrorString(result));
            // Keep the native resource under RAII if allocating its wrapper fails.
            try { clip_ = std::make_unique<FmodAudioClip>(lifetime_, sound); }
            catch (...) { static_cast<void>(sound->release()); throw; }
        }

        std::shared_ptr<FmodSystemLifetime> lifetime_;
        std::uint64_t generation_{};
        AudioLoadMode mode_;
        std::stop_source cancellation_;
        double stretchRate_{1.0};
        mutable bool converting_{};
        mutable std::uint64_t sourceDuration_{};
        mutable std::future<SourcePcm> pending_;
        mutable SourcePcm pcm_;
        mutable std::unique_ptr<FmodAudioClip> clip_;
        mutable std::string failure_;
    };
}

std::unique_ptr<IAudioClipBackend> CreateFmodPcmClip(
    FmodAudioBackend& backend, const std::filesystem::path& path,
    const AudioLoadMode mode, std::string& error)
{
    try
    {
        auto result = std::make_unique<FmodPcmClip>(backend.Lifetime(), path, mode);
        if (!IsAsync(mode) && result->LoadState(error) == AudioClipLoadState::Failed)
            return nullptr;
        error.clear();
        return result;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return nullptr;
    }
}

} // namespace PhantomCore
