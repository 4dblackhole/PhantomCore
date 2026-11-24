#pragma once

// Writes a minidump of this process when it crashes, and on request. The
// dump holds every thread's call stack and the loaded modules, not the heap,
// so it stays a few megabytes. It is written inside the process: no system
// setting is read or changed and nothing leaves the PC.

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace PhantomCore
{
enum class CrashDumpReason : std::uint8_t
{
    UnhandledException, // access violation, stack overflow, uncaught SEH
    Terminate,          // std::terminate: an exception nobody caught, a noexcept that threw
    PureVirtualCall,
    InvalidParameter,   // the C runtime rejected an argument
    Requested,          // WriteNow
};

// What the service did. dumpPath stays valid while the service lives.
struct CrashDumpReport
{
    CrashDumpReason reason{};
    // The structured exception, when there is one; zero otherwise.
    std::uint32_t exceptionCode{};
    std::uint64_t exceptionAddress{};
    // The thread that crashed or asked.
    std::uint32_t threadId{};
    bool dumpWritten{};
    const wchar_t* dumpPath{};
};

// Told after each dump attempt, on the service's own thread. After a crash the
// process is damaged and about to end: use handles opened beforehand and
// fixed buffers, do not allocate, do not take locks other threads may hold.
class ICrashDumpObserver
{
public:
    virtual void OnCrashDump(const CrashDumpReport& report) noexcept = 0;

protected:
    ~ICrashDumpObserver() = default;
};

// While one exists, a crash of this process leaves
// <directory>/<filePrefix>-YYYYMMDD-HHMMSS-<reason>.dmp before Windows ends the
// process as it always did. Only one service can exist at a time.
//
// The crashing thread only hands over to a thread the service started
// beforehand, so a stack overflow or a crash inside a lock can still be
// written. The library that writes dumps is loaded in the constructor.
class CrashDumpService final
{
public:
    // Creates the directory when it is missing and deletes the oldest dumps
    // of this prefix beyond keepNewest. Throws std::runtime_error when the
    // directory or the dump library cannot be used, std::logic_error when a
    // service already exists. observer may be null and must outlive the
    // service.
    CrashDumpService(
        const std::filesystem::path& directory,
        std::wstring_view filePrefix,
        std::uint32_t keepNewest,
        ICrashDumpObserver* observer);
    ~CrashDumpService();

    CrashDumpService(const CrashDumpService&) = delete;
    CrashDumpService& operator=(const CrashDumpService&) = delete;

    // Writes a dump of the process as it is now, from any thread, and keeps
    // running: for a stalled main loop, where the stacks say where it waits.
    // suffix names the file (ASCII letters, digits and '-'); it replaces
    // "requested" in the file name. False when the dump could not be written.
    [[nodiscard]] bool WriteNow(std::string_view suffix = {}) noexcept;
};

} // namespace PhantomCore
