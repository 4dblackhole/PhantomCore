#pragma once

// Audio feature: backend-neutral smoothing of a block-stepped mixer clock.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>


namespace PhantomCore
{
struct AudioMixerClockEstimate
{
    std::uint64_t dspClock{};
    // False when the raw clock was returned because the recent mixer
    // history was missing, stale, or inconsistent.
    bool smoothed{};
};

// A software mixer advances its DSP clock in whole blocks whenever its
// thread wakes, so a raw clock read paired with QPC carries up to one wake
// period of error. The mixer thread records each mixed block here; any
// thread can then ask for a smooth DSP clock at a QPC time.
//
// Each wake mixes whole blocks until it is a fixed target ahead of the
// device, so right after a wake the clock leads a nominal-rate line by the
// target plus less than one block. A wake observed late only lowers its
// lead, so the largest lead of the last two seconds minus one block is a
// stable estimate of the target. The estimate is that line, adjusted by half
// a block and the mean sawtooth drop between wakes so that it matches, on
// average, a raw read taken at a random moment. Results never decrease
// between calls until Reset.
class AudioMixerClockEstimator final
{
public:
    static constexpr std::size_t ObservationCapacity = 1024;

    AudioMixerClockEstimator() = default;
    AudioMixerClockEstimator(const AudioMixerClockEstimator&) = delete;
    AudioMixerClockEstimator& operator=(const AudioMixerClockEstimator&) = delete;

    // Starts a new history, for example after the mixer or its device
    // changed. Call from the estimating thread. A block recorded while this
    // runs may still be kept; the consistency checks reject it if it is off.
    void Reset(int sampleRate, std::int64_t performanceCounterFrequency) noexcept;
    // Single producer, wait-free: the mixer thread calls this once per block
    // with the QPC time it finished the block and the clock after it.
    void RecordMixedBlock(
        std::int64_t performanceCounterTicks,
        std::uint64_t dspClockAfterBlock) noexcept;
    // rawDspClock is the clock read at performanceCounterTicks. It is the
    // fallback, and a raw clock far behind the last result restarts the
    // monotonic floor (the mixer clock was reset without Reset).
    [[nodiscard]] AudioMixerClockEstimate Estimate(
        std::int64_t performanceCounterTicks,
        std::uint64_t rawDspClock) const noexcept;

private:
    struct Slot
    {
        std::atomic<std::int64_t> ticks{};
        std::atomic<std::uint64_t> clock{};
    };
    // Blocks mixed back to back form one wake. Only wakes with a known
    // neighbour on both sides are summarized, so partial wakes at either end
    // of the window cannot bias the result.
    struct WindowSummary
    {
        std::int64_t newestTicks{};
        std::uint64_t newestClock{};
        std::int64_t oldestTicks{};
        std::int64_t largestIntervalTicks{};
        // Largest (clock after a wake - newestClock) minus
        // rate * (wake time - newestTicks): how far ahead a wake mixed.
        double largestLead{-1e300};
        // Smallest clock step between blocks: the mixer block length.
        std::uint64_t blockLength{};
        // Intervals from each summarized wake to the next one.
        double intervalSum{};
        double intervalSquareSum{};
        std::size_t wakeCount{};
        std::size_t observationCount{};
    };
    struct OpenWake
    {
        std::int64_t startTicks{};
        std::int64_t lastTicks{};
        std::uint64_t lastClock{};
    };

    static void CloseWake(
        const OpenWake& wake,
        std::int64_t newerWakeStartTicks,
        double samplesPerTick,
        WindowSummary& summary) noexcept;

    [[nodiscard]] bool SummarizeRecentWindow(
        double samplesPerTick,
        std::int64_t windowTicks,
        WindowSummary& summary) const noexcept;
    [[nodiscard]] bool EstimateSmoothed(
        std::int64_t performanceCounterTicks,
        std::uint64_t rawDspClock,
        double& estimate) const noexcept;
    [[nodiscard]] std::uint64_t ApplyMonotonicFloor(
        std::uint64_t candidate,
        std::uint64_t rawDspClock) const noexcept;

    std::array<Slot, ObservationCapacity> slots_{};
    std::atomic<std::uint64_t> startedCount_{};
    std::atomic<std::uint64_t> publishedCount_{};
    std::atomic<std::uint64_t> historyStart_{};
    std::atomic<int> sampleRate_{};
    std::atomic<std::int64_t> performanceCounterFrequency_{};
    mutable std::atomic<std::uint64_t> lastEstimate_{};
};

} // namespace PhantomCore
