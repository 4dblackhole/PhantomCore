#include "AudioMixerClockTests.h"
#include "PhantomCore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>

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

    constexpr int SampleRate = 48000;
    constexpr std::int64_t TicksPerSecond = 10'000'000;

    std::int64_t ToTicks(const double seconds)
    {
        return static_cast<std::int64_t>(std::llround(seconds * TicksPerSecond));
    }

    // A software mixer that wakes on a device period and mixes whole blocks
    // until it is a fixed amount ahead of the device's linear consumption.
    class SimulatedMixer
    {
    public:
        SimulatedMixer(const std::uint32_t blockLength, const double wakeSeconds)
            : blockLength_(blockLength), wakeSeconds_(wakeSeconds) {}

        [[nodiscard]] double NextWakeSeconds() const noexcept { return nextWake_; }
        [[nodiscard]] std::uint64_t Clock() const noexcept { return clock_; }

        void Wake(AudioMixerClockEstimator& estimator)
        {
            const double now = nextWake_;
            const double target = SampleRate * now + 2.0 * blockLength_;
            int mixedBlocks = 0;
            while (static_cast<double>(clock_) < target)
            {
                clock_ += blockLength_;
                estimator.RecordMixedBlock(ToTicks(now + mixedBlocks * 20e-6), clock_);
                ++mixedBlocks;
            }
            ++wakeIndex_;
            nextWake_ = wakeIndex_ * wakeSeconds_ + NextJitterSeconds();
        }

    private:
        // Deterministic scheduling noise of up to +-0.4 ms around the
        // device-driven schedule, which itself does not drift.
        double NextJitterSeconds() noexcept
        {
            random_ = random_ * 1664525U + 1013904223U;
            return (static_cast<double>(random_ >> 8) / 16777216.0 - 0.5) * 0.8e-3;
        }

        std::uint32_t blockLength_{};
        double wakeSeconds_{};
        double nextWake_{};
        int wakeIndex_{};
        std::uint64_t clock_{};
        std::uint32_t random_{12345U};
    };

    struct ResidualRange
    {
        double minimum{1e300};
        double maximum{-1e300};
        double sum{};
        int count{};

        void Add(const double value)
        {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            sum += value;
            ++count;
        }
        [[nodiscard]] double WidthMilliseconds() const { return (maximum - minimum) * 1000.0 / SampleRate; }
        [[nodiscard]] double MeanMilliseconds() const { return sum / count * 1000.0 / SampleRate; }
    };

    struct ScenarioResult
    {
        ResidualRange raw;
        ResidualRange estimated;
        bool allSmoothed{true};
        bool monotonic{true};
    };

    // Runs the mixer for four seconds and, after a warm-up, compares raw and
    // estimated snapshots against the device's linear clock at query times
    // that are not commensurate with the wake period.
    ScenarioResult RunScenario(const std::uint32_t blockLength, const double wakeSeconds)
    {
        // The estimator holds a 16 KB ring, so keep it off the stack.
        const auto storage = std::make_unique<AudioMixerClockEstimator>();
        AudioMixerClockEstimator& estimator = *storage;
        estimator.Reset(SampleRate, TicksPerSecond);
        SimulatedMixer mixer(blockLength, wakeSeconds);
        ScenarioResult result;
        std::uint64_t previousEstimate = 0;
        for (double query = 0.0; query < 4.0; query += 0.000731)
        {
            while (mixer.NextWakeSeconds() <= query)
            {
                mixer.Wake(estimator);
            }
            const AudioMixerClockEstimate estimate =
                estimator.Estimate(ToTicks(query), mixer.Clock());
            result.monotonic = result.monotonic && estimate.dspClock >= previousEstimate;
            previousEstimate = estimate.dspClock;
            if (query < 1.5)
            {
                continue;
            }
            const double linear = SampleRate * query;
            result.raw.Add(static_cast<double>(mixer.Clock()) - linear);
            result.estimated.Add(static_cast<double>(estimate.dspClock) - linear);
            result.allSmoothed = result.allSmoothed && estimate.smoothed;
        }
        return result;
    }

    struct ScenarioLimits
    {
        double minimumRawWidthMilliseconds{};
        double maximumEstimatedWidthMilliseconds{};
        double maximumMeanShiftMilliseconds{};
    };

    void CheckScenario(
        const std::string_view name,
        const ScenarioResult& result,
        const ScenarioLimits& limits)
    {
        const double meanShift =
            result.estimated.MeanMilliseconds() - result.raw.MeanMilliseconds();
        std::cout << name << ": raw width " << result.raw.WidthMilliseconds()
                  << " ms, estimated width " << result.estimated.WidthMilliseconds()
                  << " ms, mean shift " << meanShift << " ms\n";
        Check(result.raw.WidthMilliseconds() >= limits.minimumRawWidthMilliseconds,
            "the simulated raw clock carries its block steps");
        Check(result.allSmoothed, "a warmed-up mixer history is used");
        Check(result.monotonic, "estimated clocks never decrease");
        Check(result.estimated.WidthMilliseconds() < limits.maximumEstimatedWidthMilliseconds,
            "estimated clock jitter stays within the scenario limit");
        Check(std::abs(meanShift) < limits.maximumMeanShiftMilliseconds,
            "the estimate stays near the mean of raw snapshots");
    }

    void TestFallbacks()
    {
        const auto storage = std::make_unique<AudioMixerClockEstimator>();
        AudioMixerClockEstimator& estimator = *storage;
        estimator.Reset(SampleRate, TicksPerSecond);
        Check(!estimator.Estimate(ToTicks(0.0), 777).smoothed &&
                estimator.Estimate(ToTicks(0.0), 777).dspClock == 777,
            "an empty history returns the raw clock");

        SimulatedMixer mixer(256, 0.01);
        while (mixer.NextWakeSeconds() < 0.1)
        {
            mixer.Wake(estimator);
        }
        const AudioMixerClockEstimate young =
            estimator.Estimate(ToTicks(0.1), mixer.Clock());
        Check(!young.smoothed && young.dspClock == mixer.Clock(),
            "a history shorter than the minimum window returns the raw clock");

        while (mixer.NextWakeSeconds() < 1.5)
        {
            mixer.Wake(estimator);
        }
        const double lastWake = mixer.NextWakeSeconds() - 0.01;
        const AudioMixerClockEstimate warm =
            estimator.Estimate(ToTicks(lastWake + 0.005), mixer.Clock());
        Check(warm.smoothed, "a warmed-up history is used");
        const AudioMixerClockEstimate stale =
            estimator.Estimate(ToTicks(lastWake + 0.5), mixer.Clock());
        Check(!stale.smoothed && stale.dspClock >= warm.dspClock,
            "a silent mixer falls back without moving backwards");

        const AudioMixerClockEstimate restarted =
            estimator.Estimate(ToTicks(lastWake + 0.5), 100);
        Check(restarted.dspClock == 100,
            "a raw clock far behind the floor restarts the floor");

        estimator.Reset(SampleRate, TicksPerSecond);
        const AudioMixerClockEstimate afterReset =
            estimator.Estimate(ToTicks(lastWake + 0.005), mixer.Clock());
        Check(!afterReset.smoothed && afterReset.dspClock == mixer.Clock(),
            "Reset discards earlier history and the floor");
    }
}

int TestAudioMixerClockEstimator()
{
    failureCount = 0;
    // A shared-mode device period of 10 ms with 256-sample blocks mixes one
    // or two blocks per wake, like the measured WASAPI output. The wake
    // phase sweeps the block grid, so the old raw mean is kept.
    CheckScenario("10 ms wakes, 256-sample blocks", RunScenario(256, 0.010),
        ScenarioLimits{8.0, 1.0, 0.5});
    // Wakes locked exactly on block boundaries are the worst case: noise
    // decides whether each wake mixes early or late. The raw mean then
    // depends on that phase, and the estimate stays within half a block.
    constexpr double halfBlock256 = 128.0 * 1000.0 / SampleRate;
    constexpr double halfBlock1024 = 512.0 * 1000.0 / SampleRate;
    CheckScenario("per-block wakes, 256-sample blocks", RunScenario(256, 256.0 / SampleRate),
        ScenarioLimits{4.0, 1.0, halfBlock256 + 0.5});
    // Jitter grows with the block duration but stays far below the raw steps.
    CheckScenario("per-block wakes, 1024-sample blocks", RunScenario(1024, 1024.0 / SampleRate),
        ScenarioLimits{15.0, 2.0, halfBlock1024 + 0.5});
    TestFallbacks();
    if (failureCount == 0)
    {
        std::cout << "Audio mixer clock estimator tests passed.\n";
    }
    return failureCount == 0 ? 0 : 1;
}
