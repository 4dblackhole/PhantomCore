#pragma once

// Counts the C++ exceptions thrown while it lives, for tests and smoke runs that require a normal run to throw
// none. Programs create one only for such runs, so a normal run pays nothing.
//
// Counted: throws from this executable (the engine is linked into it) and from the C++ runtime libraries it
// uses (Debug links them as DLLs), which throw on behalf of their caller. Not counted: system libraries that
// throw and catch inside themselves (Media Foundation, WIC, display drivers). A rethrow is a new throw for the
// C++ runtime and counts again: a bare `throw;` and std::rethrow_exception (as in std::future::get) alike.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace PhantomCore
{
// One counted throw. The texts are cut to fit and always end with a null character.
struct ThrownExceptionRecord
{
    std::uint32_t threadId{};
    // The thrown type's decorated name, for example ".?AVruntime_error@std@@".
    std::array<char, 96> typeName{};
    // what() when the type derives from std::exception; empty otherwise.
    std::array<char, 160> what{};
    // Return addresses from the throw upwards, the throwing machinery included.
    std::array<void*, 16> frames{};
    std::uint16_t frameCount{};
};

// While one exists, a vectored exception handler at the end of the chain counts the throws described above and
// keeps the first MaxRecords of them. It never handles an exception, allocates nothing and takes no lock, so a
// program runs and crashes exactly as without it (CrashDumpService still sees every unhandled exception).
// Only one watch can exist at a time.
class ThrownExceptionWatch final
{
public:
    static constexpr std::size_t MaxRecords = 16;

    // Throws std::logic_error when another watch exists and std::runtime_error when the handler cannot be
    // added; both before the watch counts anything.
    ThrownExceptionWatch();
    ~ThrownExceptionWatch();
    ThrownExceptionWatch(const ThrownExceptionWatch&) = delete;
    ThrownExceptionWatch& operator=(const ThrownExceptionWatch&) = delete;

    // Throws counted since construction or the last Reset.
    [[nodiscard]] std::uint64_t Count() const noexcept;
    // The first min(Count(), MaxRecords) throws. Read them while no other thread can be throwing.
    [[nodiscard]] std::span<const ThrownExceptionRecord> Records() const noexcept;
    // Starts counting again from zero, for a test that throws on purpose first.
    void Reset() noexcept;

private:
    void* handler_{};
};

} // namespace PhantomCore
