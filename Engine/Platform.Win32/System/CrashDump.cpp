#include "System/CrashDump.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <intrin.h>
#include <strsafe.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace PhantomCore
{
namespace
{
    // dbghelp.dll is loaded by name at start, so a Client needs no import
    // library and the crash path needs no loader.
    using WriteDumpFunction = BOOL(WINAPI*)(
        HANDLE,
        DWORD,
        HANDLE,
        MINIDUMP_TYPE,
        PMINIDUMP_EXCEPTION_INFORMATION,
        PMINIDUMP_USER_STREAM_INFORMATION,
        PMINIDUMP_CALLBACK_INFORMATION);

    constexpr std::size_t PathCapacity = 1024;
    constexpr std::size_t SuffixCapacity = 32;
    // How long a crashing thread waits for the dump before it lets the
    // process end anyway.
    constexpr DWORD DumpWaitMilliseconds = 20000;
    // Stack kept back for the handlers after a stack overflow on the thread
    // that created the service.
    constexpr ULONG GuaranteedStackBytes = 64 * 1024;
    // A C++ exception has its own code; std::terminate and the C runtime
    // handlers have none, so their dumps carry this one.
    constexpr DWORD NoExceptionCode = 0xE0504843;

    // Everything a crash touches lives here, sized beforehand: the crash path
    // allocates nothing. One service exists at a time, so one state does.
    struct State
    {
        WriteDumpFunction write{};
        HMODULE library{};
        ICrashDumpObserver* observer{};
        HANDLE requestEvent{};
        HANDLE doneEvent{};
        HANDLE stopEvent{};
        HANDLE thread{};
        DWORD writerThreadId{};
        wchar_t directoryAndPrefix[PathCapacity]{};
        wchar_t dumpPath[PathCapacity]{};

        // One request at a time: the thread that sets busy fills these.
        std::atomic<bool> busy{};
        CrashDumpReason reason{};
        EXCEPTION_POINTERS* exception{};
        DWORD threadId{};
        char suffix[SuffixCapacity]{};
        bool written{};

        LPTOP_LEVEL_EXCEPTION_FILTER previousFilter{};
        std::terminate_handler previousTerminate{};
        _purecall_handler previousPureCall{};
        _invalid_parameter_handler previousInvalidParameter{};

        // Normal-context only (construction and after a requested dump).
        std::filesystem::path directory;
        std::wstring prefix;
        std::uint32_t keepNewest{};
    };

    State state;
    std::atomic<bool> installed{};

    [[nodiscard]] const char* DefaultSuffix(const CrashDumpReason reason) noexcept
    {
        switch (reason)
        {
        case CrashDumpReason::UnhandledException:
            return "crash";
        case CrashDumpReason::Terminate:
            return "terminate";
        case CrashDumpReason::PureVirtualCall:
            return "purecall";
        case CrashDumpReason::InvalidParameter:
            return "invalid-parameter";
        case CrashDumpReason::Requested:
            return "requested";
        }
        return "dump";
    }

    // The writer and the observer are foreign code running in a damaged
    // process. A second fault inside them must not take the first dump along.
    [[nodiscard]] bool CallWriter(const HANDLE file, MINIDUMP_EXCEPTION_INFORMATION* exception) noexcept
    {
        __try
        {
            const auto type = static_cast<MINIDUMP_TYPE>(
                MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            return state.write(
                       GetCurrentProcess(), GetCurrentProcessId(), file, type, exception, nullptr, nullptr) != FALSE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void CallObserver(const CrashDumpReport& report) noexcept
    {
        __try
        {
            state.observer->OnCrashDump(report);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // Runs on the writer thread for the request the busy thread left in state.
    void WriteRequestedDump() noexcept
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const char* suffix = state.suffix[0] != 0 ? state.suffix : DefaultSuffix(state.reason);
        state.written = false;
        const HRESULT named = StringCchPrintfW(
            state.dumpPath,
            PathCapacity,
            L"%s-%04u%02u%02u-%02u%02u%02u-%hs.dmp",
            state.directoryAndPrefix,
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            suffix);
        if (SUCCEEDED(named))
        {
            const HANDLE file = CreateFileW(
                state.dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION exception{state.threadId, state.exception, FALSE};
                state.written = CallWriter(file, state.exception != nullptr ? &exception : nullptr);
                CloseHandle(file);
                // A file that holds no dump would only mislead.
                if (!state.written)
                    DeleteFileW(state.dumpPath);
            }
        }

        if (state.observer == nullptr)
            return;
        CrashDumpReport report;
        report.reason = state.reason;
        if (state.exception != nullptr && state.exception->ExceptionRecord != nullptr)
        {
            const EXCEPTION_RECORD& record = *state.exception->ExceptionRecord;
            report.exceptionCode = record.ExceptionCode == NoExceptionCode ? 0 : record.ExceptionCode;
            report.exceptionAddress = reinterpret_cast<std::uint64_t>(record.ExceptionAddress);
        }
        report.threadId = state.threadId;
        report.dumpWritten = state.written;
        report.dumpPath = state.dumpPath;
        CallObserver(report);
    }

    DWORD WINAPI WriterThread(void*) noexcept
    {
        const HANDLE events[]{state.stopEvent, state.requestEvent};
        while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
        {
            WriteRequestedDump();
            SetEvent(state.doneEvent);
        }
        return 0;
    }

    // Hands the request to the writer thread and waits for it. Callable from a
    // crashing thread: no allocation, no lock, almost no stack.
    bool Submit(const CrashDumpReason reason, EXCEPTION_POINTERS* exception, const char* suffix) noexcept
    {
        // A fault on the writer thread itself cannot be written by it.
        if (!installed.load() || GetCurrentThreadId() == state.writerThreadId)
            return false;
        // Another thread is being written. Its dump shows this thread too, so
        // waiting is enough; a crash does not wait forever.
        DWORD waited = 0;
        while (state.busy.exchange(true))
        {
            if (waited >= DumpWaitMilliseconds)
                return false;
            Sleep(20);
            waited += 20;
        }
        state.reason = reason;
        state.exception = exception;
        state.threadId = GetCurrentThreadId();
        state.suffix[0] = 0;
        if (suffix != nullptr)
            static_cast<void>(StringCchCopyA(state.suffix, SuffixCapacity, suffix));
        ResetEvent(state.doneEvent);
        SetEvent(state.requestEvent);
        const bool finished = WaitForSingleObject(state.doneEvent, DumpWaitMilliseconds) == WAIT_OBJECT_0;
        const bool written = finished && state.written;
        // After a timeout the writer may still be using the request.
        if (finished)
            state.busy.store(false);
        return written;
    }

    // std::terminate and the C runtime handlers come without an exception.
    // The context captured here lets the dump open on the thread and frame
    // that asked.
    __declspec(noinline) void SubmitHere(const CrashDumpReason reason) noexcept
    {
        static CONTEXT context;
        static EXCEPTION_RECORD record;
        static std::atomic<bool> used;
        if (used.exchange(true))
        {
            // A second thread arrives while the first is ending the process.
            Sleep(DumpWaitMilliseconds);
            return;
        }
        RtlCaptureContext(&context);
        record.ExceptionCode = NoExceptionCode;
        record.ExceptionAddress = _ReturnAddress();
        EXCEPTION_POINTERS pointers{&record, &context};
        static_cast<void>(Submit(reason, &pointers, nullptr));
    }

    LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exception) noexcept
    {
        static_cast<void>(Submit(CrashDumpReason::UnhandledException, exception, nullptr));
        // Windows then ends the process the way it did before the service existed.
        return state.previousFilter != nullptr ? state.previousFilter(exception) : EXCEPTION_CONTINUE_SEARCH;
    }

    [[noreturn]] void OnTerminate() noexcept
    {
        SubmitHere(CrashDumpReason::Terminate);
        if (state.previousTerminate != nullptr)
            state.previousTerminate();
        std::abort();
    }

    void OnPureVirtualCall() noexcept
    {
        SubmitHere(CrashDumpReason::PureVirtualCall);
        if (state.previousPureCall != nullptr)
            state.previousPureCall();
        std::abort();
    }

    void OnInvalidParameter(
        const wchar_t* expression,
        const wchar_t* function,
        const wchar_t* file,
        const unsigned int line,
        const uintptr_t reserved) noexcept
    {
        SubmitHere(CrashDumpReason::InvalidParameter);
        if (state.previousInvalidParameter != nullptr)
            state.previousInvalidParameter(expression, function, file, line, reserved);
        // Without a handler the runtime ends the process here; so does this.
        std::abort();
    }

    // The newest keepNewest dumps of this prefix stay. Names carry their time,
    // so name order is age order.
    void RemoveOldDumps() noexcept
    {
        try
        {
            std::vector<std::filesystem::path> dumps;
            std::error_code error;
            for (std::filesystem::directory_iterator entry(state.directory, error), end; !error && entry != end;
                 entry.increment(error))
            {
                const std::wstring name = entry->path().filename().wstring();
                const bool ours = name.starts_with(state.prefix + L"-") && entry->path().extension() == L".dmp";
                if (ours)
                    dumps.push_back(entry->path());
            }
            std::sort(dumps.begin(), dumps.end());
            const std::size_t keep = state.keepNewest;
            for (std::size_t index = 0; index + keep < dumps.size(); ++index)
                std::filesystem::remove(dumps[index], error);
        }
        catch (...)
        {
        }
    }

    [[nodiscard]] bool ValidSuffix(const std::string_view suffix) noexcept
    {
        if (suffix.size() >= SuffixCapacity)
            return false;
        return std::all_of(suffix.begin(), suffix.end(), [](const char character) {
            const bool lower = character >= 'a' && character <= 'z';
            const bool upper = character >= 'A' && character <= 'Z';
            const bool digit = character >= '0' && character <= '9';
            return lower || upper || digit || character == '-';
        });
    }

    void ReleaseState() noexcept
    {
        for (HANDLE* handle : {&state.thread, &state.requestEvent, &state.doneEvent, &state.stopEvent})
        {
            if (*handle != nullptr)
                CloseHandle(*handle);
            *handle = nullptr;
        }
        if (state.library != nullptr)
            FreeLibrary(state.library);
        state.library = nullptr;
        state.write = nullptr;
        state.observer = nullptr;
        state.writerThreadId = 0;
        state.busy.store(false);
        installed.store(false);
    }

    void PrepareWriter()
    {
        // The system copy only: a crash handler must not load a planted DLL.
        state.library = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (state.library != nullptr)
            state.write = reinterpret_cast<WriteDumpFunction>(GetProcAddress(state.library, "MiniDumpWriteDump"));
        if (state.write == nullptr)
            throw std::runtime_error("The crash dump library (dbghelp.dll) cannot be used.");

        state.requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        state.doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        state.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const bool events = state.requestEvent != nullptr && state.doneEvent != nullptr && state.stopEvent != nullptr;
        if (events)
            state.thread = CreateThread(nullptr, 0, WriterThread, nullptr, 0, &state.writerThreadId);
        if (state.thread == nullptr)
            throw std::runtime_error("The crash dump writer thread could not be started.");
    }
}

CrashDumpService::CrashDumpService(
    const std::filesystem::path& directory,
    const std::wstring_view filePrefix,
    const std::uint32_t keepNewest,
    ICrashDumpObserver* observer)
{
    if (installed.exchange(true))
        throw std::logic_error("A CrashDumpService already exists in this process.");
    try
    {
        std::filesystem::create_directories(directory);
        const std::wstring base = (directory / filePrefix).wstring();
        if (filePrefix.empty() || FAILED(StringCchCopyW(state.directoryAndPrefix, PathCapacity - 64, base.c_str())))
            throw std::runtime_error("The crash dump path is empty or too long.");
        state.directory = directory;
        state.prefix = filePrefix;
        state.keepNewest = keepNewest;
        state.observer = observer;
        RemoveOldDumps();
        PrepareWriter();
    }
    catch (...)
    {
        ReleaseState();
        throw;
    }

    ULONG guaranteed = GuaranteedStackBytes;
    static_cast<void>(SetThreadStackGuarantee(&guaranteed));
    state.previousFilter = SetUnhandledExceptionFilter(OnUnhandledException);
    state.previousTerminate = std::set_terminate(OnTerminate);
    state.previousPureCall = _set_purecall_handler(OnPureVirtualCall);
    state.previousInvalidParameter = _set_invalid_parameter_handler(OnInvalidParameter);
}

CrashDumpService::~CrashDumpService()
{
    SetUnhandledExceptionFilter(state.previousFilter);
    std::set_terminate(state.previousTerminate);
    _set_purecall_handler(state.previousPureCall);
    _set_invalid_parameter_handler(state.previousInvalidParameter);
    SetEvent(state.stopEvent);
    WaitForSingleObject(state.thread, INFINITE);
    ReleaseState();
}

bool CrashDumpService::WriteNow(const std::string_view suffix) noexcept
{
    if (!ValidSuffix(suffix))
        return false;
    char name[SuffixCapacity]{};
    std::copy(suffix.begin(), suffix.end(), name);
    const bool written = Submit(CrashDumpReason::Requested, nullptr, suffix.empty() ? nullptr : name);
    // The process goes on after a requested dump, so the limit is kept here too.
    RemoveOldDumps();
    return written;
}

} // namespace PhantomCore
