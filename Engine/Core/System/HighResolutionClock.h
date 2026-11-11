#pragma once

#include <cstdint>

// General-purpose QueryPerformanceCounter clock used by the engine loop.
namespace PhantomCore
{
class HighResolutionClock final
{
public:
    HighResolutionClock();

    [[nodiscard]] double Tick(double& totalSeconds) noexcept;
    [[nodiscard]] double NowSeconds() const noexcept;

private:
    double inverseFrequency_{};
    std::int64_t startCounter_{};
    double previousSeconds_{};
};

} // namespace PhantomCore
