#pragma once

// App feature: notices a main loop that stopped finishing passes. The loop
// counts its passes; a thread of this class looks at the count a few times a
// second and tells the diagnostic sink when it stands still for too long.

#include "Diagnostics/EngineDiagnostics.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace PhantomCore
{
class MainLoopWatch final
{
public:
    // Watches the thread that constructs it, which must own window. Does
    // nothing when sink is null or stallSeconds is zero or less.
    MainLoopWatch(HWND window, double stallSeconds, IEngineDiagnosticSink* sink);
    ~MainLoopWatch();

    MainLoopWatch(const MainLoopWatch&) = delete;
    MainLoopWatch& operator=(const MainLoopWatch&) = delete;

    // Once per pass, from the watched thread. It is the only writer, so a
    // plain store is enough and the pass pays no locked instruction.
    void Beat() noexcept
    {
        passes_.store(passes_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

private:
    void Watch() noexcept;
    [[nodiscard]] bool WindowLoopRuns() const noexcept;

    std::atomic<std::uint64_t> passes_{};
    HWND window_{};
    double stallSeconds_{};
    IEngineDiagnosticSink* sink_{};
    DWORD watchedThreadId_{};
    HANDLE stopEvent_{};
    std::thread thread_;
};

} // namespace PhantomCore
