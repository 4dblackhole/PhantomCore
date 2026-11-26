#include "BackgroundWorkTests.h"
#include "PhantomCore.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace PhantomCore;

namespace
{
    constexpr const char *TestWork = "test work";

    bool Fail(const char *message)
    {
        std::cerr << message << '\n';
        return false;
    }

    class Recorder final : public IBackgroundWorkObserver
    {
      public:
        void OnBackgroundWork(const BackgroundWorkEvent &event) noexcept override
        {
            const std::scoped_lock lock(mutex_);
            events_.push_back(event);
        }
        [[nodiscard]] std::vector<BackgroundWorkEvent> Take()
        {
            const std::scoped_lock lock(mutex_);
            return std::exchange(events_, {});
        }

      private:
        std::mutex mutex_;
        std::vector<BackgroundWorkEvent> events_;
    };

    [[nodiscard]] std::size_t RunningCount()
    {
        std::array<RunningBackgroundWork, BackgroundWork::MaxRunning + 8> running{};
        return BackgroundWork::Running(running);
    }

    // A piece that another thread holds open is listed with its name and that thread, and is gone once it ends.
    bool CheckRunning(Recorder &recorder)
    {
        std::atomic<bool> started{}, release{};
        std::uint32_t workerId = 0;
        std::thread worker([&] {
            workerId = GetCurrentThreadId();
            const BackgroundWork work(TestWork);
            started.store(true);
            while (!release.load())
                std::this_thread::yield();
        });
        while (!started.load())
            std::this_thread::yield();
        std::array<RunningBackgroundWork, 4> running{};
        const std::size_t count = BackgroundWork::Running(running);
        const bool listed = count == 1 && std::string_view(running[0].name) == TestWork;
        const bool described = listed && running[0].threadId == workerId && running[0].milliseconds >= 0.0;
        release.store(true);
        worker.join();
        if (!described)
            return Fail("A running piece is listed with its name, thread and time.");
        if (RunningCount() != 0)
            return Fail("A piece that ended is no longer listed.");
        const auto events = recorder.Take();
        if (events.size() != 2 || events[0].finished || !events[1].finished)
            return Fail("The observer hears one start and one end.");
        const bool sameWork = std::string_view(events[1].name) == TestWork && events[0].threadId == workerId;
        if (!sameWork || events[1].threadId != workerId)
            return Fail("Both events name the work and the worker's thread.");
        if (events[1].outcome != BackgroundWorkOutcome::Succeeded || events[1].milliseconds < 0.0)
            return Fail("A piece that just ends succeeded, with the time it took.");
        return true;
    }

    [[nodiscard]] BackgroundWorkOutcome LastOutcome(Recorder &recorder)
    {
        const auto events = recorder.Take();
        return events.empty() ? BackgroundWorkOutcome::Succeeded : events.back().outcome;
    }

    [[nodiscard]] bool Named(const BackgroundWorkOutcome outcome, const std::string_view name)
    {
        return BackgroundWorkOutcomeName(outcome) == name;
    }

    bool CheckOutcomes(Recorder &recorder)
    {
        {
            BackgroundWork work(TestWork);
            work.Cancelled();
        }
        if (LastOutcome(recorder) != BackgroundWorkOutcome::Cancelled)
            return Fail("A piece the worker calls cancelled ends as cancelled.");
        {
            BackgroundWork work(TestWork);
            work.Failed();
        }
        if (LastOutcome(recorder) != BackgroundWorkOutcome::Failed)
            return Fail("A piece the worker calls failed ends as failed.");
        try
        {
            const BackgroundWork work(TestWork);
            throw std::runtime_error("the work failed");
        }
        catch (const std::runtime_error &)
        {
        }
        if (LastOutcome(recorder) != BackgroundWorkOutcome::Failed)
            return Fail("A piece that an exception leaves ends as failed.");
        // A piece inside a handler is not failed by the exception that is already being handled.
        try
        {
            throw std::runtime_error("handled outside the work");
        }
        catch (const std::runtime_error &)
        {
            const BackgroundWork work(TestWork);
        }
        if (LastOutcome(recorder) != BackgroundWorkOutcome::Succeeded)
            return Fail("An exception caught before the piece began does not fail it.");
        const bool named = Named(BackgroundWorkOutcome::Succeeded, "succeeded") &&
                           Named(BackgroundWorkOutcome::Failed, "failed") &&
                           Named(BackgroundWorkOutcome::Cancelled, "cancelled");
        return named || Fail("Every outcome has its name.");
    }

    // More pieces than the table holds: the list is cut, every piece still reaches the observer, and the
    // table is empty again afterwards.
    bool CheckLimit(Recorder &recorder)
    {
        constexpr std::size_t Pieces = BackgroundWork::MaxRunning + 3;
        {
            std::vector<std::unique_ptr<BackgroundWork>> pieces;
            for (std::size_t index = 0; index < Pieces; ++index)
                pieces.push_back(std::make_unique<BackgroundWork>(TestWork));
            if (RunningCount() != BackgroundWork::MaxRunning)
                return Fail("The list holds MaxRunning pieces.");
            std::array<RunningBackgroundWork, 2> few{};
            if (BackgroundWork::Running(few) != few.size())
                return Fail("The list is cut to the room it is given.");
        }
        if (RunningCount() != 0)
            return Fail("The table is empty after every piece ended.");
        if (recorder.Take().size() != Pieces * 2)
            return Fail("A piece the table has no room for still reaches the observer.");
        return true;
    }

    bool CheckSingleObservation(Recorder &recorder)
    {
        try
        {
            Recorder other;
            const BackgroundWorkObservation second(other);
            return Fail("A second observation is refused.");
        }
        catch (const std::logic_error &)
        {
        }
        {
            const BackgroundWork work(TestWork);
        }
        return recorder.Take().size() == 2 || Fail("The first observation keeps hearing after a refused second.");
    }

    int Report(const char *message)
    {
        return Fail(message) ? 0 : 1;
    }
} // namespace

int TestBackgroundWork()
{
    if (RunningCount() != 0)
        return Report("Nothing runs before the first piece.");
    Recorder recorder;
    {
        const BackgroundWorkObservation observation(recorder);
        const bool marks = CheckRunning(recorder) && CheckOutcomes(recorder);
        if (!marks || !CheckLimit(recorder) || !CheckSingleObservation(recorder))
            return 1;
    }
    // Without an observation a piece is still listed, and nobody is told.
    {
        const BackgroundWork work(TestWork);
        if (RunningCount() != 1)
            return Report("A piece is listed without an observer.");
    }
    if (!recorder.Take().empty())
        return Report("An ended observation hears nothing more.");
    Recorder again;
    const BackgroundWorkObservation next(again);
    std::cout << "Background work marks passed: running list, start and end events, outcomes, table limit of "
              << BackgroundWork::MaxRunning << ", one observation at a time.\n";
    return 0;
}
