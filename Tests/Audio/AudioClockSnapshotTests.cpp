#include "AudioClockSnapshotTests.h"
#include "PhantomCore.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace PhantomCore;

namespace
{
    constexpr int WarmUpMilliseconds = 1500;
    constexpr int SnapshotCount = 400;
    constexpr double MaximumSmoothedWidthMilliseconds = 2.0;

    std::int64_t Now()
    {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    std::int64_t Frequency()
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart;
    }

    // Keeps the audio service updated about once per millisecond, like the
    // engine loop, until the given QPC time.
    void PumpUntil(AudioSystem& audio, const std::int64_t endTicks)
    {
        const std::int64_t updateTicks = Frequency() / 1000;
        std::int64_t lastUpdate = 0;
        for (std::int64_t now = Now(); now < endTicks; now = Now())
        {
            if (now - lastUpdate >= updateTicks)
            {
                audio.Update();
                lastUpdate = now;
            }
        }
    }

    struct ClockPair
    {
        std::int64_t ticks{};
        std::uint64_t dspClock{};
    };

    struct JitterStatistics
    {
        double widthMilliseconds{};
        double standardDeviationMilliseconds{};
        double meanMilliseconds{};
    };

    // Residuals against a least-squares line, so device-versus-QPC drift
    // does not count as jitter. The mean is against the nominal-rate line
    // through the shared origin, so means of two series are comparable.
    JitterStatistics Measure(
        const std::vector<ClockPair>& pairs,
        const ClockPair& origin,
        const int sampleRate)
    {
        const double ticksPerSecond = static_cast<double>(Frequency());
        const std::int64_t originTicks = origin.ticks;
        const std::uint64_t originClock = origin.dspClock;
        std::vector<double> seconds;
        std::vector<double> residuals;
        for (const ClockPair& pair : pairs)
        {
            const double time = static_cast<double>(pair.ticks - originTicks) / ticksPerSecond;
            const double clock = static_cast<double>(static_cast<std::int64_t>(pair.dspClock - originClock));
            seconds.push_back(time);
            residuals.push_back((clock - sampleRate * time) * 1000.0 / sampleRate);
        }

        const auto count = static_cast<double>(pairs.size());
        double meanTime = 0.0;
        double meanResidual = 0.0;
        for (std::size_t index = 0; index < pairs.size(); ++index)
        {
            meanTime += seconds[index] / count;
            meanResidual += residuals[index] / count;
        }
        double covariance = 0.0;
        double variance = 0.0;
        for (std::size_t index = 0; index < pairs.size(); ++index)
        {
            covariance += (seconds[index] - meanTime) * (residuals[index] - meanResidual);
            variance += (seconds[index] - meanTime) * (seconds[index] - meanTime);
        }
        const double slope = variance > 0.0 ? covariance / variance : 0.0;

        double minimum = 1e300;
        double maximum = -1e300;
        double squares = 0.0;
        for (std::size_t index = 0; index < pairs.size(); ++index)
        {
            const double detrended = residuals[index] - meanResidual -
                slope * (seconds[index] - meanTime);
            minimum = std::min(minimum, detrended);
            maximum = std::max(maximum, detrended);
            squares += detrended * detrended;
        }
        return JitterStatistics{maximum - minimum, std::sqrt(squares / count), meanResidual};
    }

    void Print(const char* label, const JitterStatistics& statistics)
    {
        std::cout << label << ": width " << statistics.widthMilliseconds
                  << " ms, standard deviation " << statistics.standardDeviationMilliseconds
                  << " ms\n";
    }
}

int TestAudioClockSnapshot()
{
    AudioSystem audio;
    std::string error;
    if (!audio.Initialize(AudioConfig{}, nullptr, nullptr, error))
    {
        std::cerr << "FAILED: audio initialization: " << error << '\n';
        return 1;
    }
    const int sampleRate = audio.SampleRate();
    std::cout << "Output " << static_cast<int>(audio.ActiveOutput()) << ", "
              << sampleRate << " Hz, DSP buffer " << audio.DspBufferLength()
              << " x " << audio.DspBufferCount() << '\n';
    PumpUntil(audio, Now() + Frequency() * WarmUpMilliseconds / 1000);

    // Each sample pairs QPC with the raw clock exactly as snapshots did
    // before smoothing, then takes a snapshot. Waits of 1.3 to 8.7 ms are
    // not commensurate with any mixer period.
    std::vector<ClockPair> raw;
    std::vector<ClockPair> snapshots;
    int smoothedCount = 0;
    bool monotonic = true;
    std::uint32_t random = 2463534242U;
    for (int index = 0; index < SnapshotCount; ++index)
    {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        const double waitMilliseconds = 1.3 + (random % 7400U) / 1000.0;
        PumpUntil(audio, Now() + static_cast<std::int64_t>(Frequency() * waitMilliseconds / 1000.0));

        const std::int64_t rawTicks = Now();
        raw.push_back(ClockPair{rawTicks, audio.DspClock()});
        const AudioClockSnapshot snapshot = audio.CaptureClockSnapshot();
        monotonic = monotonic &&
            (snapshots.empty() || snapshot.dspClock >= snapshots.back().dspClock);
        snapshots.push_back(ClockPair{snapshot.performanceCounterTicks, snapshot.dspClock});
        smoothedCount += snapshot.smoothed ? 1 : 0;
    }
    audio.Shutdown();

    const JitterStatistics before = Measure(raw, raw.front(), sampleRate);
    const JitterStatistics after = Measure(snapshots, raw.front(), sampleRate);
    Print("Raw clock pairing (before)", before);
    Print("CaptureClockSnapshot (after)", after);
    std::cout << "Mean shift " << after.meanMilliseconds - before.meanMilliseconds
              << " ms, smoothed " << smoothedCount << " of " << SnapshotCount << '\n';

    int failures = 0;
    const auto check = [&failures](const bool condition, const char* description)
    {
        if (!condition)
        {
            ++failures;
            std::cerr << "FAILED: " << description << '\n';
        }
    };
    check(smoothedCount == SnapshotCount, "every warmed-up snapshot is smoothed");
    check(monotonic, "snapshots never decrease");
    check(after.widthMilliseconds < MaximumSmoothedWidthMilliseconds,
        "snapshot jitter stays within two milliseconds");
    check(after.widthMilliseconds <= before.widthMilliseconds,
        "snapshot jitter is no wider than the raw clock");
    if (failures == 0)
    {
        std::cout << "Audio clock snapshot tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
