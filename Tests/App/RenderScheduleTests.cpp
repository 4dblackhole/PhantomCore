#include "RenderScheduleTests.h"
#include "PhantomCore.h"

#include <cmath>
#include <cstdint>
#include <iostream>
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

    // Runs a loop that iterates every `step` seconds for `seconds` and
    // returns how many iterations rendered.
    std::uint64_t CountFrames(RenderSchedule &schedule, const double start, const double seconds, const double step)
    {
        std::uint64_t frames = 0;
        const auto iterations = static_cast<std::uint64_t>(std::llround(seconds / step));
        for (std::uint64_t index = 0; index < iterations; ++index)
        {
            const double now = start + static_cast<double>(index) * step;
            if (!schedule.IsDue(now))
                continue;
            ++frames;
            schedule.MarkRendered(now);
        }
        return frames;
    }

    bool Near(const std::uint64_t value, const double expected)
    {
        return std::abs(static_cast<double>(value) - expected) <= 1.0;
    }

    void TestLimitedModes()
    {
        // A 59.94 Hz display and a loop iterating 20,000 times a second.
        constexpr double DisplayRate = 60000.0 / 1001.0;
        constexpr double Step = 0.00005;
        RenderSchedule schedule;
        schedule.SetBaseRate(DisplayRate, 0.0);
        Check(schedule.Mode() == RenderRateMode::Display, "The default mode renders once per display refresh.");
        Check(std::abs(schedule.IntervalSeconds() * DisplayRate - 1.0) < 1e-12,
              "The Display interval is one refresh period of the exact rate.");
        Check(Near(CountFrames(schedule, 0.0, 10.0, Step), DisplayRate * 10.0),
              "Display mode renders the display rate, not its whole-number part.");

        schedule.SetMode(RenderRateMode::DisplayDouble, 10.0);
        Check(std::abs(schedule.IntervalSeconds() * DisplayRate - 0.5) < 1e-12,
              "The DisplayDouble interval is half a refresh period.");
        Check(Near(CountFrames(schedule, 10.0, 10.0, Step), DisplayRate * 20.0),
              "DisplayDouble mode renders twice the display rate.");

        schedule.SetBaseRate(144.0, 20.0);
        Check(Near(CountFrames(schedule, 20.0, 5.0, Step), 144.0 * 10.0),
              "A display rate change keeps the mode and uses the new rate.");
        schedule.SetBaseRate(0.5, 25.0);
        Check(schedule.BaseRateHz() == 144.0, "A rate of 1 Hz or less is ignored.");
    }

    void TestUnlimitedMode()
    {
        RenderSchedule schedule;
        schedule.SetBaseRate(60.0, 0.0);
        schedule.SetMode(RenderRateMode::Unlimited, 0.0);
        Check(schedule.IntervalSeconds() == 0.0, "Unlimited mode has no interval.");
        Check(CountFrames(schedule, 0.0, 1.0, 0.001) == 1000, "Unlimited mode renders on every loop iteration.");

        // Back to a limited mode: the deadline restarts at the switch
        // instead of replaying the time spent unlimited.
        schedule.SetMode(RenderRateMode::Display, 1.0);
        Check(schedule.IsDue(1.0), "The first frame after a mode change is due at once.");
        Check(Near(CountFrames(schedule, 1.0, 1.0, 0.001), 60.0), "A limited mode after Unlimited renders its own rate.");
    }

    void TestLateFrame()
    {
        RenderSchedule schedule;
        schedule.SetBaseRate(100.0, 0.0);
        schedule.MarkRendered(0.0);
        Check(!schedule.IsDue(0.005) && schedule.IsDue(0.010), "The next frame is due one interval later.");
        // The loop stalled for half a second: no burst of catch-up frames.
        schedule.MarkRendered(0.500);
        Check(!schedule.IsDue(0.505), "A stalled loop does not render a catch-up burst.");
        Check(schedule.IsDue(0.510), "After a stall the deadline restarts one interval after the late frame.");
    }

    // A 360 Hz display behind another program's window, in each mode.
    void TestBackgroundCap()
    {
        constexpr double Step = 0.0001;
        constexpr RenderRateMode Modes[] = {
            RenderRateMode::Display, RenderRateMode::DisplayDouble, RenderRateMode::Unlimited};
        for (const RenderRateMode mode : Modes)
        {
            RenderSchedule schedule;
            schedule.SetBaseRate(360.0, 0.0);
            schedule.SetMode(mode, 0.0);
            schedule.SetBackgroundRate(DefaultBackgroundRenderRateHz);
            Check(!schedule.IsBackgroundLimited(), "The cap waits until the window is in the background.");
            const std::uint64_t front = CountFrames(schedule, 0.0, 1.0, Step);
            Check(front > 300, "In front the mode renders its own rate.");

            schedule.SetBackground(true, 1.0);
            Check(schedule.IsBackgroundLimited(), "Behind another window the cap applies.");
            Check(Near(CountFrames(schedule, 1.0, 10.0, Step), DefaultBackgroundRenderRateHz * 10.0),
                  "Behind another window every mode renders 30 frames a second.");

            // Brought back in the middle of a background interval.
            schedule.MarkRendered(11.0);
            schedule.SetBackground(false, 11.001);
            Check(schedule.IsDue(11.001), "A window brought to the front is drawn at once.");
            Check(Near(CountFrames(schedule, 11.001, 1.0, Step), static_cast<double>(front)),
                  "In front again the mode renders its own rate.");
        }
    }

    void TestBackgroundCapLimits()
    {
        RenderSchedule slow;
        slow.SetBaseRate(24.0, 0.0);
        slow.SetBackgroundRate(DefaultBackgroundRenderRateHz);
        slow.SetBackground(true, 0.0);
        Check(Near(CountFrames(slow, 0.0, 10.0, 0.0001), 240.0), "A rate slower than the cap keeps its own rate.");

        RenderSchedule off;
        off.SetBaseRate(60.0, 0.0);
        off.SetMode(RenderRateMode::Unlimited, 0.0);
        off.SetBackgroundRate(0.0);
        off.SetBackground(true, 0.0);
        Check(!off.IsBackgroundLimited(), "A cap of 0 is off.");
        Check(CountFrames(off, 0.0, 1.0, 0.001) == 1000, "With the cap off the background renders every iteration.");
    }
} // namespace

int TestRenderSchedule()
{
    failureCount = 0;
    TestLimitedModes();
    TestUnlimitedMode();
    TestLateFrame();
    TestBackgroundCap();
    TestBackgroundCapLimits();
    return failureCount;
}
