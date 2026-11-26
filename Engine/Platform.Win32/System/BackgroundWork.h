#pragma once

// Which work runs outside the main loop right now, and when each piece started and ended. It is for diagnosing
// a hang or a lost graphics device after the fact: the question then is what else the process was doing.
//
// A worker puts one BackgroundWork on its stack around a piece of work. That keeps the piece in a process-wide
// table that any thread may read at any moment, a crash or stall handler included, and tells the observer (if a
// program installed one) that the piece started and ended. Without an observer a piece costs two atomic writes.

#include <cstddef>
#include <cstdint>
#include <span>

namespace PhantomCore
{
enum class BackgroundWorkOutcome : std::uint8_t
{
    Succeeded,
    Failed,    // an exception left the piece, or the worker called Failed
    Cancelled, // the worker was told to stop and called Cancelled
};

[[nodiscard]] const char* BackgroundWorkOutcomeName(BackgroundWorkOutcome outcome) noexcept;

// The start (finished is false) or the end of one piece of work.
struct BackgroundWorkEvent
{
    const char* name{}; // has static storage
    std::uint32_t threadId{};
    bool finished{};
    // Only for an end.
    BackgroundWorkOutcome outcome{};
    double milliseconds{};
};

// One piece that has started and not ended.
struct RunningBackgroundWork
{
    const char* name{};
    std::uint32_t threadId{};
    double milliseconds{}; // since it started
};

// Called on the worker's own thread, at the start and at the end of each piece. It must not throw and should
// return quickly: the work waits for it.
class IBackgroundWorkObserver
{
public:
    virtual void OnBackgroundWork(const BackgroundWorkEvent& event) noexcept = 0;

protected:
    ~IBackgroundWorkObserver() = default;
};

// While one exists, observer hears of every piece of work in the process. Only one can exist at a time. The
// destructor returns after the last call into observer has returned, so the observer may be destroyed next.
class BackgroundWorkObservation final
{
public:
    // Throws std::logic_error when another observation exists.
    explicit BackgroundWorkObservation(IBackgroundWorkObserver& observer);
    ~BackgroundWorkObservation();
    BackgroundWorkObservation(const BackgroundWorkObservation&) = delete;
    BackgroundWorkObservation& operator=(const BackgroundWorkObservation&) = delete;
};

// One piece of work, from construction to destruction, on the thread that does it. It allocates nothing and
// takes no lock.
class BackgroundWork final
{
public:
    // Pieces running at the same time that the table holds. One more still reaches the observer but is missing
    // from Running.
    static constexpr std::size_t MaxRunning = 32;

    // name must have static storage (a string literal) and says what the work is, not which file it reads.
    explicit BackgroundWork(const char* name) noexcept;
    // The piece ends here. It failed when an exception is leaving through it.
    ~BackgroundWork();
    BackgroundWork(const BackgroundWork&) = delete;
    BackgroundWork& operator=(const BackgroundWork&) = delete;

    // For work that reports its ending by value. The last call decides; an exception still means Failed.
    void Cancelled() noexcept
    {
        outcome_ = BackgroundWorkOutcome::Cancelled;
    }
    void Failed() noexcept
    {
        outcome_ = BackgroundWorkOutcome::Failed;
    }

    // Copies the pieces that run now into running and returns how many there are (at most running.size()).
    // Safe from any thread at any moment; a piece that starts or ends meanwhile may or may not be included.
    [[nodiscard]] static std::size_t Running(std::span<RunningBackgroundWork> running) noexcept;

private:
    const char* name_;
    std::int64_t startTicks_;
    std::size_t slot_;
    int exceptions_;
    BackgroundWorkOutcome outcome_{BackgroundWorkOutcome::Succeeded};
};

} // namespace PhantomCore
