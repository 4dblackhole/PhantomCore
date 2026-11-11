#include "PhantomCore.h"
#include "AudioPcmTests.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace PhantomCore;

namespace
{
    void Require(bool value, const std::string& message)
    {
        if (!value)
            throw std::runtime_error(message);
    }
    struct PcmProbe
    {
        std::uint32_t channels{}, rate{};
        std::uint64_t frames{}, attackFrame{UINT64_MAX};
        std::vector<float> samples;
    };
    PcmProbe Probe(const std::filesystem::path& path)
    {
        PcmProbe pcm;
        DecodeAudioPcm(path, {}, [&](std::uint32_t channels, std::uint32_t rate,
                                               std::span<const float> samples)
        {
            Require(channels > 0 && rate > 0 && samples.size() % channels == 0,
                    "Decoder provides complete PCM frames and a source format.");
            Require(samples.size() / channels <= 4096, "Decoder delivers bounded chunks.");
            pcm.channels = channels;
            pcm.rate = rate;
            for (std::size_t i = 0; i < samples.size(); ++i)
                if (pcm.attackFrame == UINT64_MAX && std::abs(samples[i]) > .0316227766F)
                    pcm.attackFrame = pcm.frames + i / channels;
            pcm.frames += samples.size() / channels;
            pcm.samples.insert(pcm.samples.end(), samples.begin(), samples.end());
        });
        return pcm;
    }
    void WaitReady(AudioSystem& audio, AudioClip& clip)
    {
        std::string error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (true)
        {
            audio.Update();
            const auto state = clip.LoadState(error);
            Require(state != AudioClipLoadState::Failed, error);
            if (state != AudioClipLoadState::Loading)
                return;
            Require(std::chrono::steady_clock::now() < deadline, "PCM clip loading timed out.");
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    void WaitVoice(AudioSystem& audio, AudioVoice& voice)
    {
        std::string error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (true)
        {
            audio.Update();
            const auto state = voice.PreparationState(error);
            Require(state != AudioPreparationState::Failed, error);
            if (state == AudioPreparationState::Ready)
                return;
            Require(std::chrono::steady_clock::now() < deadline, "PCM source seeking timed out.");
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    void VerifyStretch()
    {
        constexpr std::uint32_t rate = 24000;
        std::vector<float> tone(rate * 2);
        for (std::size_t frame = 0; frame < rate; ++frame)
        {
            tone[frame * 2] = static_cast<float>(0.5 * std::sin(frame * 2.0 * 3.141592653589793 * 440 / rate));
            tone[frame * 2 + 1] = tone[frame * 2] * 0.5F;
        }
        for (double speed : {0.5, 1.0, 1.5, 2.0})
        {
            const auto output = TimeStretchPcm(tone, 2, rate, speed, {});
            const auto frames = output.size() / 2;
            Require(frames == static_cast<std::size_t>(std::llround(rate / speed)), "Stretch produces exact rounded duration.");
            std::size_t crossings{};
            double stereoError{};
            const auto begin = frames / 4, end = frames * 3 / 4;
            for (auto i = begin; i < end; ++i)
            {
                Require(std::isfinite(output[i * 2]), "Stretch output is finite.");
                if (output[(i - 1) * 2] < 0 && output[i * 2] >= 0) ++crossings;
                stereoError += std::abs(output[i * 2 + 1] - output[i * 2] * .5F);
            }
            const double frequency = static_cast<double>(crossings) * rate / (end - begin);
            Require(std::abs(frequency - 440) < 8, "Stretch keeps pitch independently of rate.");
            Require(stereoError / (end - begin) < .002, "Stretch preserves coherent stereo channels.");
        }
        // A short burst at 0.4 s detects algorithm latency left in the output.
        std::vector<float> burst(rate, 0);
        for (std::size_t i = rate * 4 / 10; i < rate * 41 / 100; ++i) burst[i] = .8F;
        for (double speed : {0.5, 1.5, 2.0})
        {
            const auto output = TimeStretchPcm(burst, 1, rate, speed, {});
            const auto peak = std::max_element(output.begin(), output.end());
            const double seconds = static_cast<double>(peak - output.begin()) / rate;
            Require(std::abs(seconds - .405 / speed) < .05, "Time stretch removes algorithm latency at transients.");
        }
        std::stop_source stop; stop.request_stop();
        bool cancelled{};
        try { static_cast<void>(TimeStretchPcm(tone, 2, rate, 1.5, stop.get_token())); }
        catch (const std::runtime_error &) { cancelled = true; }
        Require(cancelled, "Cancelled stretch never publishes partial PCM.");
    }
    void VerifyPlayback(const std::filesystem::path& path, const PcmProbe& pcm)
    {

        AudioSystem audio;
        AudioConfig config;
        config.preferredBackend = AudioOutputBackend::NoSound;
        std::string error;
        Require(audio.Initialize(config, CreateFmodAudioBackend, CreateFmodAudioClipBackend, error), error);
        for (double speed : {0.5, 1.5, 2.0})
        {
            auto clip = audio.LoadSound(path, AudioLoadMode::StreamAsyncAccurate, error);
            Require(clip && clip->PrepareTimeStretch(speed, error), error);
            WaitReady(audio, *clip);
            std::uint64_t duration{};
            Require(clip->GetDurationMicroseconds(duration, error), error);
            Require(duration == static_cast<std::uint64_t>(std::llround(static_cast<long double>(pcm.frames) * 1'000'000 / pcm.rate)),
                    "Stretched clips expose original source duration.");
            AudioPlaybackSettings settings; settings.startPaused = true; settings.startPositionMicroseconds = duration / 3;
            auto voice = clip->Play(settings, nullptr, error);
            Require(voice != nullptr, error); WaitVoice(audio, *voice);
            settings.startPositionMicroseconds = duration - 1;
            Require(voice->Restart(settings, nullptr, error), error); WaitVoice(audio, *voice);
            settings.startPositionMicroseconds = duration;
            Require(!voice->Restart(settings, nullptr, error), "Stretched source seeks reject the source end.");
            Require(voice->Stop(error), error);
            voice.reset(); clip.reset();
        }
        for (auto mode : {AudioLoadMode::Sample, AudioLoadMode::SampleAsync, AudioLoadMode::StreamAsyncAccurate})
        {
            auto clip = audio.LoadSound(path, mode, error);
            Require(clip != nullptr, error);
            WaitReady(audio, *clip);
            std::uint64_t duration{};
            Require(clip->GetDurationMicroseconds(duration, error), error);
            const auto expected = static_cast<std::uint64_t>(std::llround(
                static_cast<long double>(pcm.frames) * 1'000'000 / pcm.rate));
            Require(duration == expected, "Playback and analysis expose exactly the same PCM frame count.");
            AudioPlaybackSettings settings;
            settings.startPaused = true;
            settings.startPositionMicroseconds = duration / 3;
            auto voice = clip->Play(settings, nullptr, error);
            Require(voice != nullptr, error);
            WaitVoice(audio, *voice);
            for (auto position : {std::uint64_t{0}, duration / 2, duration - 1, std::uint64_t{0}})
            {
                settings.startPositionMicroseconds = position;
                Require(voice->Restart(settings, nullptr, error), error);
                WaitVoice(audio, *voice);
            }
            Require(voice->Stop(error), error);
            settings.startPositionMicroseconds = duration;
            Require(clip->Play(settings, nullptr, error) == nullptr,
                    "The normalized PCM end position is rejected before allocating a voice.");
        }
        // Cancellation/shutdown must not leave a decoder referencing the device.
        auto cancelled = audio.LoadSound(path, AudioLoadMode::StreamAsyncAccurate, error);
        audio.RetireClip(std::move(cancelled));
        audio.Shutdown();
    }
}

int TestAudioPcm(const char* source)
{
    try
    {
        VerifyStretch();
        const auto fixture = std::filesystem::path(__FILE__).parent_path() / "Fixtures/PaddedTone.mp3";
        auto pcm = Probe(fixture);
        Require(pcm.rate == 44100 && pcm.channels == 2 && pcm.frames == 44100,
                "LAME priming and end padding are removed while preserving the exact source length.");
        const double attack = static_cast<double>(pcm.attackFrame) * 1000 / pcm.rate;
        Require(attack >= 249 && attack < 252,
                "Intentional 250 ms leading silence is preserved without an MP3 frame shift.");
        VerifyPlayback(fixture, pcm);
        std::stop_source stop;
        stop.request_stop();
        bool called{};
        DecodeAudioPcm("must-not-open-a-cancelled-file", stop.get_token(),
                                  [&](auto, auto, auto) { called = true; });
        Require(!called, "Cancelled reads do not open the file or invoke consumers.");
        std::stop_source partial;
        std::uint64_t frames{};
        DecodeAudioPcm(fixture, partial.get_token(), [&](auto channels, auto, auto samples)
        {
            frames += samples.size() / channels;
            if (frames)
                partial.request_stop();
        });
        Require(frames == 4096, "Cancellation stops after the first delivered PCM block.");
        if (source)
        {
            const std::filesystem::path path(std::u8string_view{reinterpret_cast<const char8_t*>(source)});
            pcm = Probe(path);
            VerifyPlayback(path, pcm);
            std::cout << "Source rate=" << pcm.rate << " channels=" << pcm.channels
                      << " frames=" << pcm.frames << " first_attack_ms="
                      << static_cast<double>(pcm.attackFrame) * 1000 / pcm.rate << '\n';
        }
        std::cout << "PCM decoding, exact duration, async loading, seek/restart and cancellation passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "PCM test failed: " << error.what() << '\n';
        return 1;
    }
}
