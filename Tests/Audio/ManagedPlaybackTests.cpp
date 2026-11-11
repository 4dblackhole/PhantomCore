#include "ManagedPlaybackTests.h"
#include "PhantomCore.h"
#include <iostream>
#include <string_view>
#include <utility>

using namespace PhantomCore;

namespace
{
    int failureCount = 0;

    void Check(const bool condition, const std::string_view description)
    {
        if (!condition)
        {
            ++failureCount;
            std::cerr << "FAILED: " << description << '\n';
        }
    }

    struct PlaybackProbe
    {
        bool playing{true};
        bool paused{};
        bool failStop{};
        bool finishOnRestart{};
        bool failRestart{};
        int stops{};
        int restarts{};
        std::uint64_t startDspClock{};
        std::uint64_t startPositionMicroseconds{};
        IAudioBusBackend* restartBus{};
    };
    std::vector<std::shared_ptr<PlaybackProbe>> playbackProbes;
    bool failTestPlayback{};
    AudioClipLoadState testClipState{AudioClipLoadState::Ready};

    class TestVoice final : public IAudioVoiceBackend
    {
    public:
        explicit TestVoice(std::shared_ptr<PlaybackProbe> probe)
            : probe_(std::move(probe)) {}
        bool IsPlaying() const noexcept override { return probe_->playing; }
        bool Restart(
            const AudioPlaybackSettings& settings,
            IAudioBusBackend* bus,
            std::string& error) override
        {
            if (probe_->finishOnRestart)
            {
                probe_->playing = false;
                error = "ended during restart";
                return false;
            }
            if (probe_->failRestart)
            {
                error = "active restart failure";
                return false;
            }
            ++probe_->restarts;
            probe_->playing = true;
            probe_->paused = settings.startPaused;
            probe_->startDspClock = settings.startDspClock;
            probe_->startPositionMicroseconds = settings.startPositionMicroseconds;
            probe_->restartBus = bus;
            return true;
        }
        bool Stop(std::string& error) override
        {
            if (probe_->failStop)
            {
                error = "test stop failure";
                return false;
            }
            ++probe_->stops;
            probe_->playing = false;
            return true;
        }
        bool SetPaused(bool paused, std::string&) override
        {
            probe_->paused = paused;
            return true;
        }
        bool SetVolume(float, std::string&) override { return true; }
        bool SetPitch(float, std::string&) override { return true; }
        bool SetDelay(std::uint64_t start, std::uint64_t, std::string&) override
        { probe_->startDspClock = start; return true; }
    private:
        std::shared_ptr<PlaybackProbe> probe_;
    };

    class TestClip final : public IAudioClipBackend
    {
    public:
        bool GetDurationMicroseconds(std::uint64_t& duration, std::string&) const override
        { duration = 1'000'000; return true; }
        AudioClipLoadState LoadState(std::string&) const override { return testClipState; }
        std::unique_ptr<IAudioVoiceBackend> Play(
            const AudioPlaybackSettings& settings,
            IAudioBusBackend*, std::string& error) override
        {
            if (failTestPlayback)
            {
                error = "test playback failure";
                return nullptr;
            }
            auto probe = std::make_shared<PlaybackProbe>();
            probe->paused = settings.startPaused;
            probe->startDspClock = settings.startDspClock;
            probe->startPositionMicroseconds = settings.startPositionMicroseconds;
            playbackProbes.push_back(probe);
            return std::make_unique<TestVoice>(std::move(probe));
        }
    };

    std::unique_ptr<IAudioClipBackend> CreateTestClip(
        IAudioBackend&, const std::filesystem::path&,
        AudioLoadMode, std::string&)
    {
        return std::make_unique<TestClip>();
    }

    void TestManagedAudioPlayback()
    {

        AudioSystem audio;
        AudioConfig config;
        config.preferredBackend = AudioOutputBackend::NoSound;
        std::string error;
        const bool initialized = audio.Initialize(
            config, nullptr, &CreateTestClip, error);
        Check(initialized, "audio test initializes without an output device");
        if (!initialized) { return; }

        AudioPlaybackManager manager;
        Check(manager.Play(nullptr, {}, nullptr, error) == InvalidAudioPlaybackId,
            "managed playback rejects null clips");
        std::shared_ptr<AudioClip> clip = audio.LoadSound("probe", error);
        std::shared_ptr<AudioBus> bus = audio.CreateBus("probe", nullptr, error);
        Check(clip != nullptr && bus != nullptr, "audio test creates resources");
        if (clip == nullptr || bus == nullptr) { return; }
        std::uint64_t duration = 0;
        Check(clip->GetDurationMicroseconds(duration, error) && duration == 1'000'000,
            "clip forwards source duration in microseconds");
        const std::weak_ptr<AudioClip> weakClip = clip;
        const std::weak_ptr<AudioBus> weakBus = bus;
        AudioPlaybackSettings settings;
        settings.startPaused = true;
        settings.startDspClock = 123456;
        settings.startPositionMicroseconds = 456789;
        const auto first = manager.Play(clip, settings, bus, error);
        const auto second = manager.Play(clip, {}, bus, error);
        Check(first != 0 && second != 0 && first != second,
            "overlapping playback has independent IDs");
        if (first == 0 || second == 0) { return; }
        clip.reset();
        bus.reset();
        manager.Update();
        Check(!weakClip.expired() && !weakBus.expired() &&
            manager.PlaybackCount() == 2 && playbackProbes[0]->paused &&
            playbackProbes[0]->startDspClock == 123456,
            "manager retains assets and paused/scheduled voices after owner releases them");
        Check(playbackProbes[0]->startPositionMicroseconds == 456789 &&
            manager.FindVoice(first)->SetDelay(222222, 0, error) &&
            playbackProbes[0]->startDspClock == 222222,
            "source seek and prepared-voice DSP start are forwarded independently");
        Check(manager.FindVoice(first)->SetPaused(false, error) &&
            !playbackProbes[0]->paused, "managed voice supports resume");
        std::shared_ptr<AudioBus> restartBus =
            audio.CreateBus("restart", nullptr, error);
        const std::weak_ptr<AudioBus> weakRestartBus = restartBus;
        AudioPlaybackSettings restartSettings;
        restartSettings.startPaused = true;
        restartSettings.startDspClock = 654321;
        restartSettings.startPositionMicroseconds = 1234;
        Check(manager.Restart(
                first, restartSettings, restartBus, error) &&
            manager.PlaybackCount() == 2 &&
            playbackProbes.size() == 2 &&
            playbackProbes[0]->restarts == 1 &&
            playbackProbes[0]->paused &&
            playbackProbes[0]->startDspClock == 654321 &&
            playbackProbes[0]->startPositionMicroseconds == 1234 &&
            playbackProbes[0]->restartBus != nullptr,
            "managed playback restarts its existing voice without allocating another one");
        restartBus.reset();
        Check(!weakRestartBus.expired(),
            "restarted playback retains its replacement bus");
        playbackProbes[0]->failStop = true;
        Check(!manager.Stop(first, error) && manager.FindVoice(first) != nullptr,
            "failed stop keeps the voice controllable");
        playbackProbes[0]->failStop = false;
        Check(manager.Stop(first, error) && playbackProbes[0]->stops == 1 &&
            playbackProbes[1]->playing && manager.FindVoice(first) == nullptr,
            "stopping one playback explicitly stops only that voice");
        Check(weakRestartBus.expired(),
            "stopping restarted playback releases its replacement bus");
        Check(!manager.Restart(first, {}, nullptr, error),
            "stale playback IDs cannot restart another voice");
        Check(!manager.Stop(first, error), "stale playback IDs cannot stop another voice");
        playbackProbes[1]->playing = false;
        manager.Update();
        Check(manager.PlaybackCount() == 0 && weakClip.expired() && weakBus.expired(),
            "natural completion releases manager resources");

        clip = audio.LoadSound("probe", error);
        failTestPlayback = true;
        Check(manager.Play(clip, {}, nullptr, error) == 0 &&
            manager.PlaybackCount() == 0 && error == "test playback failure",
            "failed playback leaves no entry and preserves the backend error");
        failTestPlayback = false;
        const auto third = manager.Play(clip, {}, nullptr, error);
        manager.StopAll();
        const auto fourth = manager.Play(clip, {}, nullptr, error);
        Check(third > second && fourth > third && playbackProbes[2]->stops == 1,
            "StopAll explicitly stops voices without reusing IDs");
        manager.StopAll();
        Check(manager.PlaybackCount() == 0 && playbackProbes[3]->stops == 1,
            "shutdown clears remaining managed playback");
        {
            AudioPlaybackManager temporary;
            Check(temporary.Play(clip, {}, nullptr, error) != 0,
                "temporary manager starts playback");
        }
        Check(playbackProbes.back()->stops == 1,
            "manager destructor explicitly stops playback");
        const auto recovered = manager.Play(clip, {}, nullptr, error);
        const auto ended = playbackProbes.back();
        ended->finishOnRestart = true;
        Check(manager.Restart(recovered, settings, nullptr, error) &&
                  manager.PlaybackCount() == 1 && playbackProbes.back() != ended && error.empty(),
              "a mixer ending the channel during Restart recreates a voice under the same ID");
        const auto live = playbackProbes.back();
        live->failRestart = true;
        Check(!manager.Restart(recovered, settings, nullptr, error) &&
                  error == "active restart failure" && playbackProbes.back() == live,
              "an error on a live channel is preserved rather than hidden by replacement");
        manager.StopAll();
        playbackProbes.clear();
        std::shared_ptr<AudioClip> retired = audio.LoadSound("retired", error);
        const std::weak_ptr<AudioClip> weakRetired = retired;
        testClipState = AudioClipLoadState::Loading;
        audio.RetireClip(std::move(retired));
        audio.Update();
        Check(!weakRetired.expired(), "retired loading clips survive until native I/O settles");
        testClipState = AudioClipLoadState::Ready;
        audio.Update();
        Check(weakRetired.expired(), "settled retired clips release on the audio update");
    }

}

int TestManagedPlayback()
{
    failureCount = 0;
    TestManagedAudioPlayback();
    if (failureCount != 0) return 1;
    std::cout << "All managed audio playback tests passed.\n";
    return 0;
}
