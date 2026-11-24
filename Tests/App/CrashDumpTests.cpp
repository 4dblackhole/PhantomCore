#include "CrashDumpTests.h"
#include "PhantomCore.h"
#include <Windows.h>
#include <crtdbg.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace PhantomCore;

namespace
{
    constexpr wchar_t DumpPrefix[] = L"test";
    constexpr DWORD ChildWaitMilliseconds = 60000;

    // Writes what the service reported into a file opened before the crash,
    // the way a Client's observer has to: no allocation, no lock.
    class MarkerObserver final : public ICrashDumpObserver
    {
      public:
        explicit MarkerObserver(const std::filesystem::path &path)
            : file_(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr))
        {
        }
        ~MarkerObserver()
        {
            if (file_ != INVALID_HANDLE_VALUE)
                CloseHandle(file_);
        }
        MarkerObserver(const MarkerObserver &) = delete;
        MarkerObserver &operator=(const MarkerObserver &) = delete;

        void OnCrashDump(const CrashDumpReport &report) noexcept override
        {
            char line[96]{};
            const int length = sprintf_s(line, "reason=%d written=%d code=%08X\n", static_cast<int>(report.reason),
                                         report.dumpWritten ? 1 : 0, report.exceptionCode);
            DWORD written = 0;
            if (file_ != INVALID_HANDLE_VALUE && length > 0)
                WriteFile(file_, line, static_cast<DWORD>(length), &written, nullptr);
        }

      private:
        HANDLE file_;
    };

    // A crashing child must end without a dialog that would hold the test.
    void SilenceCrashDialogs()
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _CrtSetReportMode(_CRT_ASSERT, 0);
        _CrtSetReportMode(_CRT_ERROR, 0);
    }

    __declspec(noinline) int Recurse(volatile int *keepGoing)
    {
        volatile char pad[4096];
        pad[0] = 1;
        return *keepGoing != 0 ? Recurse(keepGoing) + pad[0] : 0;
    }

    struct PureBase
    {
        PureBase()
        {
            CallThrough();
        }
        virtual ~PureBase() = default;
        virtual void Pure() = 0;
        // Not inlined, so the call stays a virtual one made during construction.
        __declspec(noinline) void CallThrough()
        {
            Pure();
        }
    };
    struct PureDerived final : PureBase
    {
        void Pure() override
        {
        }
    };

    void CrashAs(const std::string_view kind)
    {
        if (kind == "crash")
            RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        if (kind == "stack-overflow")
        {
            volatile int keepGoing = 1;
            std::exit(Recurse(&keepGoing));
        }
        if (kind == "terminate")
            std::terminate();
        if (kind == "purecall")
        {
            PureDerived object;
            object.Pure();
        }
        if (kind == "invalid-parameter")
        {
            // A destination too small for its text is what the runtime rejects.
            char small[2]{};
            volatile std::size_t size = sizeof(small);
            strcpy_s(small, size, "too long for two bytes");
        }
    }

    // "requested": three dumps in a service that keeps two.
    int WriteRequestedDumps(CrashDumpService &service)
    {
        const bool first = service.WriteNow("a");
        const bool second = service.WriteNow("b");
        const bool third = service.WriteNow("stall");
        const bool refused = !service.WriteNow("not allowed!");
        const bool written = first && second && third;
        return written && refused ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    struct Expectation
    {
        const char *kind;
        const wchar_t *suffix;
        int reason;
        bool crashes;
        std::size_t dumps;
    };

    constexpr Expectation Expectations[]{
        {"crash", L"crash", 0, true, 1},
        {"stack-overflow", L"crash", 0, true, 1},
        {"terminate", L"terminate", 1, true, 1},
        {"purecall", L"purecall", 2, true, 1},
        {"invalid-parameter", L"invalid-parameter", 3, true, 1},
        {"requested", L"stall", 4, false, 2},
    };

    bool Fail(const std::string &message)
    {
        std::cerr << message << '\n';
        return false;
    }

    // The parent names the test folder by its process id; the child rebuilds
    // the same path, so no path travels through the narrow command line.
    std::filesystem::path TestRoot(const DWORD parentProcessId)
    {
        return std::filesystem::temp_directory_path() /
               (L"PhantomCoreCrashDumpTest-" + std::to_wstring(parentProcessId));
    }

    bool RunChild(const std::string_view kind, DWORD &exitCode)
    {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --crash-dump-child " +
                               std::wstring(kind.begin(), kind.end()) + L" " +
                               std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &process))
            return false;
        const bool ended = WaitForSingleObject(process.hProcess, ChildWaitMilliseconds) == WAIT_OBJECT_0;
        if (!ended)
            TerminateProcess(process.hProcess, 99);
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return ended;
    }

    bool IsMinidump(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        char signature[4]{};
        file.read(signature, sizeof(signature));
        return file.gcount() == 4 && std::memcmp(signature, "MDMP", 4) == 0 && std::filesystem::file_size(path) > 4096;
    }

    bool CheckChild(const Expectation &expected, const std::filesystem::path &root)
    {
        const std::string kind = expected.kind;
        const std::filesystem::path directory = root / kind;
        DWORD exitCode = 0;
        if (!RunChild(kind, exitCode))
            return Fail(kind + ": the child did not end in time.");
        if (expected.crashes == (exitCode == 0))
            return Fail(kind + ": unexpected exit code " + std::to_string(exitCode) + ".");

        std::vector<std::filesystem::path> dumps;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.path().extension() == L".dmp")
                dumps.push_back(entry.path());
        }
        if (dumps.size() != expected.dumps)
            return Fail(kind + ": expected " + std::to_string(expected.dumps) + " dump files, found " +
                        std::to_string(dumps.size()) + ".");
        bool suffixFound = false;
        for (const auto &dump : dumps)
        {
            if (!IsMinidump(dump))
                return Fail(kind + ": a dump file is not a minidump.");
            suffixFound = suffixFound || dump.stem().wstring().ends_with(std::wstring(L"-") + expected.suffix);
        }
        if (!suffixFound)
            return Fail(kind + ": no dump file carries the expected reason in its name.");

        std::ifstream marker(directory / L"observer.txt");
        std::string line;
        std::string last;
        while (std::getline(marker, line))
            last = line;
        const std::string wanted = "reason=" + std::to_string(expected.reason) + " written=1";
        if (!last.starts_with(wanted))
            return Fail(kind + ": the observer was told \"" + last + "\", expected \"" + wanted + "\".");
        std::cout << "  " << kind << ": exit " << exitCode << ", " << dumps.size() << " dump(s), "
                  << std::filesystem::file_size(dumps.back()) / 1024 << " KB, observer: " << last << '\n';
        return true;
    }
} // namespace

int RunCrashDumpChild(const char *kind, const char *parentProcessId)
{
    SilenceCrashDialogs();
    const std::filesystem::path directory = TestRoot(std::strtoul(parentProcessId, nullptr, 10)) / kind;
    std::filesystem::create_directories(directory);
    MarkerObserver observer(directory / L"observer.txt");
    CrashDumpService service(directory, DumpPrefix, 2, &observer);
    if (std::string_view(kind) == "requested")
        return WriteRequestedDumps(service);
    CrashAs(kind);
    // Reaching this line means the crash did not happen.
    return EXIT_SUCCESS;
}

int TestCrashDump()
{
    const std::filesystem::path root = TestRoot(GetCurrentProcessId());
    std::error_code error;
    std::filesystem::remove_all(root, error);
    bool passed = true;
    for (const Expectation &expected : Expectations)
        passed = CheckChild(expected, root) && passed;
    std::filesystem::remove_all(root, error);
    if (!passed)
        return EXIT_FAILURE;
    std::cout << "Crash dumps passed: unhandled exception, stack overflow, terminate, pure call, invalid parameter, "
                 "requested dumps and the keep-newest limit.\n";
    return EXIT_SUCCESS;
}
