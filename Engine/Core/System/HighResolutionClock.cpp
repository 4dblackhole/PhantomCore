#include "System/HighResolutionClock.h"

#include <Windows.h>

#include <stdexcept>

namespace PhantomCore
{
HighResolutionClock::HighResolutionClock()
{
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&start))
    {
        throw std::runtime_error(
            "The high-resolution performance counter is unavailable.");
    }

    inverseFrequency_ =
        1.0 / static_cast<double>(frequency.QuadPart);
    startCounter_ = static_cast<std::int64_t>(start.QuadPart);
    previousSeconds_ = 0.0;
}

double HighResolutionClock::Tick(double& totalSeconds) noexcept
{
    totalSeconds = NowSeconds();
    const double delta = totalSeconds - previousSeconds_;
    previousSeconds_ = totalSeconds;
    return delta;
}

double HighResolutionClock::NowSeconds() const noexcept
{
    LARGE_INTEGER current{};
    QueryPerformanceCounter(&current);
    return static_cast<double>(
        static_cast<std::int64_t>(current.QuadPart) -
        startCounter_) * inverseFrequency_;
}

} // namespace PhantomCore
