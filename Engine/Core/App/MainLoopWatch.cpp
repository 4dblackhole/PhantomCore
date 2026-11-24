#include "App/MainLoopWatch.h"

#include <stdexcept>

namespace PhantomCore
{
namespace
{
    // The stall is measured in looks at the counter, not in clock time, so a
    // PC that slept does not come back with a stall it never had.
    constexpr DWORD LookMilliseconds = 250;
    constexpr UINT WindowAnswerMilliseconds = 500;
}

MainLoopWatch::MainLoopWatch(const HWND window, const double stallSeconds, IEngineDiagnosticSink* sink)
    : window_(window), stallSeconds_(stallSeconds), sink_(sink), watchedThreadId_(GetCurrentThreadId())
{
    if (sink_ == nullptr || stallSeconds_ <= 0.0)
        return;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr)
        throw std::runtime_error("CreateEventW failed for the main loop watch.");
    try
    {
        thread_ = std::thread([this] { Watch(); });
    }
    catch (...)
    {
        CloseHandle(stopEvent_);
        throw;
    }
}

MainLoopWatch::~MainLoopWatch()
{
    if (stopEvent_ == nullptr)
        return;
    SetEvent(stopEvent_);
    thread_.join();
    CloseHandle(stopEvent_);
}

// The window's own loops (moving, sizing, a menu, a modal dialog) hold the
// main loop for as long as the user likes while still answering messages. A
// thread that is really stuck answers nothing.
bool MainLoopWatch::WindowLoopRuns() const noexcept
{
    if (window_ == nullptr)
        return false;
    DWORD_PTR answer = 0;
    return SendMessageTimeoutW(window_, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, WindowAnswerMilliseconds, &answer) != 0;
}

void MainLoopWatch::Watch() noexcept
{
    std::uint64_t seen = passes_.load(std::memory_order_relaxed);
    double stillSeconds = 0.0;
    bool reported = false;
    while (WaitForSingleObject(stopEvent_, LookMilliseconds) == WAIT_TIMEOUT)
    {
        const std::uint64_t passes = passes_.load(std::memory_order_relaxed);
        if (passes != seen)
        {
            if (reported)
                sink_->OnMainLoopResumed(stillSeconds);
            seen = passes;
            stillSeconds = 0.0;
            reported = false;
            continue;
        }

        stillSeconds += LookMilliseconds / 1000.0;
        if (reported || stillSeconds < stallSeconds_)
            continue;
        // A breakpoint stops the loop on purpose, and a window loop is the user's doing.
        if (IsDebuggerPresent() || WindowLoopRuns())
            continue;
        reported = true;
        sink_->OnMainLoopStalled({stillSeconds, watchedThreadId_, passes});
    }
}

} // namespace PhantomCore
