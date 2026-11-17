#include "System/AudioMixerClock.h"

#include <algorithm>
#include <cmath>


namespace PhantomCore
{
namespace
{
    // The averaging window must span many wakes so that the phase of the
    // block pattern within a wake and scheduling noise average out.
    constexpr double WindowSeconds = 2.0;
    constexpr double MinimumWindowSeconds = 0.2;
    constexpr double MinimumWindowWakes = 8.0;
    constexpr std::size_t MinimumWakes = 8;
    // Extrapolating past a silent mixer is not justified; fall back instead.
    constexpr double StaleFloorSeconds = 0.1;
    constexpr double StaleWakes = 4.0;
    // A smooth value farther than this from the raw clock means the history
    // no longer describes the mixer.
    constexpr double PlausibleFloorSeconds = 0.02;
    constexpr double PlausibleWakes = 4.0;
    // Blocks mixed closer together than this fraction of a block's duration
    // belong to the same wake of the mixer thread.
    constexpr double SameWakeBlockFraction = 0.25;
    // A raw clock this far behind the monotonic floor means a new mixer clock.
    constexpr double FloorRestartSeconds = 1.0;
    // Slots the producer could overwrite during one read are never read.
    constexpr std::size_t ReaderHeadroom = 128;
}

void AudioMixerClockEstimator::Reset(
    const int sampleRate,
    const std::int64_t performanceCounterFrequency) noexcept
{
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    performanceCounterFrequency_.store(
        performanceCounterFrequency,
        std::memory_order_relaxed);
    historyStart_.store(
        publishedCount_.load(std::memory_order_acquire),
        std::memory_order_relaxed);
    lastEstimate_.store(0, std::memory_order_relaxed);
}

void AudioMixerClockEstimator::RecordMixedBlock(
    const std::int64_t performanceCounterTicks,
    const std::uint64_t dspClockAfterBlock) noexcept
{
    // Seqlock order: announce the overwrite before touching the slot, so a
    // reader that sees the new slot contents also sees startedCount_ move.
    const std::uint64_t index = publishedCount_.load(std::memory_order_relaxed);
    startedCount_.store(index + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    Slot& slot = slots_[index % ObservationCapacity];
    slot.ticks.store(performanceCounterTicks, std::memory_order_relaxed);
    slot.clock.store(dspClockAfterBlock, std::memory_order_relaxed);
    publishedCount_.store(index + 1, std::memory_order_release);
}

AudioMixerClockEstimate AudioMixerClockEstimator::Estimate(
    const std::int64_t performanceCounterTicks,
    const std::uint64_t rawDspClock) const noexcept
{
    double smoothed = 0.0;
    const bool useSmoothed = EstimateSmoothed(
        performanceCounterTicks,
        rawDspClock,
        smoothed);
    const std::uint64_t candidate = useSmoothed
        ? static_cast<std::uint64_t>(std::llround(smoothed))
        : rawDspClock;
    return AudioMixerClockEstimate{
        ApplyMonotonicFloor(candidate, rawDspClock),
        useSmoothed};
}

void AudioMixerClockEstimator::CloseWake(
    const OpenWake& wake,
    const std::int64_t newerWakeStartTicks,
    const double samplesPerTick,
    WindowSummary& summary) noexcept
{
    const double clockOffset = static_cast<double>(
        static_cast<std::int64_t>(wake.lastClock - summary.newestClock));
    const auto timeOffset =
        static_cast<double>(wake.lastTicks - summary.newestTicks);
    summary.largestLead = std::max(
        summary.largestLead,
        clockOffset - samplesPerTick * timeOffset);

    const std::int64_t interval = newerWakeStartTicks - wake.startTicks;
    summary.intervalSum += static_cast<double>(interval);
    summary.intervalSquareSum +=
        static_cast<double>(interval) * static_cast<double>(interval);
    summary.largestIntervalTicks = std::max(summary.largestIntervalTicks, interval);
    summary.oldestTicks = wake.startTicks;
    ++summary.wakeCount;
}

bool AudioMixerClockEstimator::SummarizeRecentWindow(
    const double samplesPerTick,
    const std::int64_t windowTicks,
    WindowSummary& summary) const noexcept
{
    const std::uint64_t published =
        publishedCount_.load(std::memory_order_acquire);
    const std::uint64_t historyStart =
        historyStart_.load(std::memory_order_relaxed);
    if (published <= historyStart)
    {
        return false;
    }
    const std::uint64_t readable = std::min<std::uint64_t>(
        published - historyStart,
        ObservationCapacity - ReaderHeadroom);

    const auto readSlot = [this](const std::uint64_t index, std::int64_t& ticks, std::uint64_t& clock)
    {
        const Slot& slot = slots_[index % ObservationCapacity];
        ticks = slot.ticks.load(std::memory_order_relaxed);
        clock = slot.clock.load(std::memory_order_relaxed);
    };

    // Walk from the newest block back through the window, grouping blocks
    // mixed back to back into wakes. The newest wake may still be mixing and
    // the oldest may be cut by the window, so neither is summarized.
    std::int64_t newerTicks = 0;
    std::uint64_t newerClock = 0;
    readSlot(published - 1, newerTicks, newerClock);
    summary = WindowSummary{};
    summary.newestTicks = newerTicks;
    summary.newestClock = newerClock;
    summary.oldestTicks = newerTicks;
    summary.observationCount = 1;
    OpenWake wake{newerTicks, newerTicks, newerClock};
    bool wakeHasNewerNeighbour = false;
    std::int64_t newerWakeStartTicks = 0;
    for (std::uint64_t back = 2; back <= readable; ++back)
    {
        std::int64_t olderTicks = 0;
        std::uint64_t olderClock = 0;
        readSlot(published - back, olderTicks, olderClock);
        const bool ordered = olderTicks <= newerTicks && olderClock < newerClock;
        if (!ordered || summary.newestTicks - olderTicks > windowTicks)
        {
            break;
        }

        const std::uint64_t step = newerClock - olderClock;
        summary.blockLength = summary.blockLength == 0
            ? step
            : std::min(summary.blockLength, step);
        const double blockTicks = static_cast<double>(step) / samplesPerTick;
        const auto gapTicks = static_cast<double>(newerTicks - olderTicks);
        if (gapTicks <= SameWakeBlockFraction * blockTicks)
        {
            wake.startTicks = olderTicks;
        }
        else
        {
            if (wakeHasNewerNeighbour)
            {
                CloseWake(wake, newerWakeStartTicks, samplesPerTick, summary);
            }
            wakeHasNewerNeighbour = true;
            newerWakeStartTicks = wake.startTicks;
            wake = OpenWake{olderTicks, olderTicks, olderClock};
        }
        ++summary.observationCount;
        newerTicks = olderTicks;
        newerClock = olderClock;
    }

    // Reject the read if the producer began overwriting any slot it used.
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t started = startedCount_.load(std::memory_order_relaxed);
    const std::uint64_t oldestIndex = published - summary.observationCount;
    return oldestIndex + ObservationCapacity >= started;
}

bool AudioMixerClockEstimator::EstimateSmoothed(
    const std::int64_t performanceCounterTicks,
    const std::uint64_t rawDspClock,
    double& estimate) const noexcept
{
    const int sampleRate = sampleRate_.load(std::memory_order_relaxed);
    const std::int64_t frequency =
        performanceCounterFrequency_.load(std::memory_order_relaxed);
    if (sampleRate <= 0 || frequency <= 0)
    {
        return false;
    }
    const double ticksPerSecond = static_cast<double>(frequency);
    const double samplesPerTick = static_cast<double>(sampleRate) / ticksPerSecond;
    const auto windowTicks =
        static_cast<std::int64_t>(WindowSeconds * ticksPerSecond);

    WindowSummary summary;
    if (!SummarizeRecentWindow(samplesPerTick, windowTicks, summary))
    {
        return false;
    }

    // Require enough wakes, recent enough, to trust the averaged lead.
    const auto spanTicks =
        static_cast<double>(summary.newestTicks - summary.oldestTicks);
    const auto largestInterval = static_cast<double>(summary.largestIntervalTicks);
    const double requiredSpan = std::max(
        MinimumWindowSeconds * ticksPerSecond,
        MinimumWindowWakes * largestInterval);
    if (summary.wakeCount < MinimumWakes || spanTicks < requiredSpan)
    {
        return false;
    }
    const auto sinceNewest = static_cast<double>(
        performanceCounterTicks - summary.newestTicks);
    if (sinceNewest > std::max(StaleFloorSeconds * ticksPerSecond, StaleWakes * largestInterval))
    {
        return false;
    }

    // The post-wake lead averages the target plus half a block. Between
    // wakes the raw clock stands still while the line advances, so its time
    // average sits below that lead by rate * E[I^2] / 2E[I] for intervals I.
    const auto blockLength = static_cast<double>(summary.blockLength);
    const double meanLead = summary.largestLead - 0.5 * blockLength;
    const double sawtoothDrop = samplesPerTick *
        summary.intervalSquareSum / (2.0 * summary.intervalSum);
    estimate = static_cast<double>(summary.newestClock) +
        meanLead - sawtoothDrop + samplesPerTick * sinceNewest;
    const double tolerance = samplesPerTick * std::max(
        PlausibleFloorSeconds * ticksPerSecond,
        PlausibleWakes * largestInterval);
    return estimate >= 0.0 &&
        std::abs(estimate - static_cast<double>(rawDspClock)) <= tolerance;
}

std::uint64_t AudioMixerClockEstimator::ApplyMonotonicFloor(
    const std::uint64_t candidate,
    const std::uint64_t rawDspClock) const noexcept
{
    const int sampleRate = sampleRate_.load(std::memory_order_relaxed);
    const auto restartTolerance = static_cast<std::uint64_t>(
        std::max(0, sampleRate) * FloorRestartSeconds);
    std::uint64_t previous = lastEstimate_.load(std::memory_order_relaxed);
    for (;;)
    {
        const bool clockRestarted = rawDspClock + restartTolerance < previous;
        const std::uint64_t floor = clockRestarted ? 0 : previous;
        if (candidate <= floor)
        {
            return floor;
        }
        if (lastEstimate_.compare_exchange_weak(
                previous,
                candidate,
                std::memory_order_relaxed))
        {
            return candidate;
        }
    }
}

} // namespace PhantomCore
