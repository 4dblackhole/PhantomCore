#include "System/BackgroundWork.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>

namespace PhantomCore
{
namespace
{
    constexpr std::size_t NoSlot = BackgroundWork::MaxRunning;

    // A slot is taken by writing the thread id over zero, and shown to readers by writing the name last. It is
    // given back in the opposite order, so a reader never sees a name with another piece's start time.
    struct Slot
    {
        std::atomic<std::uint32_t> threadId{};
        std::atomic<std::int64_t> startTicks{};
        std::atomic<const char*> name{};
    };

    std::array<Slot, BackgroundWork::MaxRunning> slots;
    std::atomic<IBackgroundWorkObserver*> observer{};
    // Calls into the observer that have begun and not returned.
    std::atomic<std::uint32_t> observerCalls{};

    [[nodiscard]] std::int64_t Ticks() noexcept
    {
        LARGE_INTEGER ticks{};
        QueryPerformanceCounter(&ticks);
        return ticks.QuadPart;
    }

    [[nodiscard]] double MillisecondsSince(const std::int64_t startTicks) noexcept
    {
        // Asked every time: a stall handler may be the first caller, and a guarded static would be a lock.
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(Ticks() - startTicks) * 1000.0 / static_cast<double>(frequency.QuadPart);
    }

    [[nodiscard]] std::size_t TakeSlot(const char* name, const std::uint32_t threadId,
                                       const std::int64_t startTicks) noexcept
    {
        for (std::size_t index = 0; index < slots.size(); ++index)
        {
            std::uint32_t free = 0;
            if (!slots[index].threadId.compare_exchange_strong(free, threadId))
                continue;
            slots[index].startTicks.store(startTicks);
            slots[index].name.store(name);
            return index;
        }
        return NoSlot;
    }

    void Tell(const BackgroundWorkEvent& event) noexcept
    {
        // Counted before the observer is read: an observation that ends waits for this call.
        observerCalls.fetch_add(1);
        if (IBackgroundWorkObserver* const current = observer.load())
            current->OnBackgroundWork(event);
        observerCalls.fetch_sub(1);
    }
} // namespace

const char* BackgroundWorkOutcomeName(const BackgroundWorkOutcome outcome) noexcept
{
    static constexpr std::array<const char*, 3> Names{"succeeded", "failed", "cancelled"};
    const auto index = static_cast<std::size_t>(outcome);
    return index < Names.size() ? Names[index] : "unknown";
}

BackgroundWorkObservation::BackgroundWorkObservation(IBackgroundWorkObserver& target)
{
    IBackgroundWorkObserver* none = nullptr;
    if (!observer.compare_exchange_strong(none, &target))
        throw std::logic_error("Another background work observation exists.");
}

BackgroundWorkObservation::~BackgroundWorkObservation()
{
    observer.store(nullptr);
    while (observerCalls.load() != 0)
        std::this_thread::yield();
}

BackgroundWork::BackgroundWork(const char* name) noexcept
    : name_(name), startTicks_(Ticks()), slot_(TakeSlot(name, GetCurrentThreadId(), startTicks_)),
      exceptions_(std::uncaught_exceptions())
{
    Tell({name_, GetCurrentThreadId(), false, {}, 0.0});
}

BackgroundWork::~BackgroundWork()
{
    if (std::uncaught_exceptions() > exceptions_)
        outcome_ = BackgroundWorkOutcome::Failed;
    if (slot_ != NoSlot)
    {
        slots[slot_].name.store(nullptr);
        slots[slot_].threadId.store(0);
    }
    Tell({name_, GetCurrentThreadId(), true, outcome_, MillisecondsSince(startTicks_)});
}

std::size_t BackgroundWork::Running(const std::span<RunningBackgroundWork> running) noexcept
{
    std::size_t count = 0;
    for (const Slot& slot : slots)
    {
        if (count == running.size())
            break;
        const char* const name = slot.name.load();
        if (name == nullptr)
            continue;
        running[count++] = {name, slot.threadId.load(), MillisecondsSince(slot.startTicks.load())};
    }
    return count;
}

} // namespace PhantomCore
