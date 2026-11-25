#include "ThrownExceptionWatchTests.h"
#include "PhantomCore.h"
#include <Windows.h>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace PhantomCore;

namespace
{
    bool Fail(const char *message)
    {
        std::cerr << message << '\n';
        return false;
    }

    // A function the optimizer cannot fold into its caller, so a recorded stack has a frame to name.
    __declspec(noinline) void ThrowAndCatch(const char *what)
    {
        try
        {
            throw std::runtime_error(what);
        }
        catch (const std::runtime_error &)
        {
        }
    }

    bool CheckQuiet(const ThrownExceptionWatch &watch)
    {
        if (watch.Count() != 0 || !watch.Records().empty())
            return Fail("A watch starts with no throws.");
        return true;
    }

    bool CheckOwnThrow(ThrownExceptionWatch &watch)
    {
        ThrowAndCatch("watched throw");
        if (watch.Count() != 1 || watch.Records().size() != 1)
            return Fail("A throw caught by the same code is counted once.");
        const auto &record = watch.Records().front();
        if (std::string_view(record.what.data()) != "watched throw")
            return Fail("The record keeps what() of a std::exception type.");
        if (std::string_view(record.typeName.data()) != ".?AVruntime_error@std@@")
            return Fail("The record names the thrown type.");
        if (record.threadId != GetCurrentThreadId() || record.frameCount == 0)
            return Fail("The record names the throwing thread and keeps its stack.");
        return true;
    }

    bool CheckOtherThreadAndType(ThrownExceptionWatch &watch)
    {
        watch.Reset();
        std::uint32_t workerId = 0;
        std::thread worker([&workerId] {
            workerId = GetCurrentThreadId();
            try
            {
                throw 42;
            }
            catch (int)
            {
            }
        });
        worker.join();
        if (watch.Count() != 1)
            return Fail("A throw on another thread is counted.");
        const auto &record = watch.Records().front();
        if (record.threadId != workerId || record.what[0] != '\0')
            return Fail("A throw that is not a std::exception has its thread and no what().");
        if (std::string_view(record.typeName.data()) != ".H")
            return Fail("A thrown int is named by its decorated type.");
        return true;
    }

    // A rethrow is a new throw for the C++ runtime: a bare `throw;` and std::rethrow_exception
    // (std::future::get) both count again.
    bool CheckRethrows(ThrownExceptionWatch &watch)
    {
        watch.Reset();
        try
        {
            try
            {
                throw std::logic_error("passed on");
            }
            catch (const std::logic_error &)
            {
                throw;
            }
        }
        catch (const std::logic_error &)
        {
        }
        if (watch.Count() != 2)
            return Fail("A bare rethrow counts as a throw of its own.");
        std::exception_ptr stored;
        try
        {
            throw std::runtime_error("stored");
        }
        catch (...)
        {
            stored = std::current_exception();
        }
        try
        {
            std::rethrow_exception(stored);
        }
        catch (const std::runtime_error &)
        {
        }
        if (watch.Count() != 4)
            return Fail("std::rethrow_exception counts as a throw of its own.");
        return true;
    }

    bool CheckLimitAndReset(ThrownExceptionWatch &watch)
    {
        watch.Reset();
        for (int i = 0; i < 20; ++i)
            ThrowAndCatch("many");
        if (watch.Count() != 20 || watch.Records().size() != ThrownExceptionWatch::MaxRecords)
            return Fail("Every throw is counted; only the first MaxRecords are kept.");
        watch.Reset();
        if (watch.Count() != 0 || !watch.Records().empty())
            return Fail("Reset starts from zero.");
        return true;
    }

    bool CheckSingleWatch(ThrownExceptionWatch &watch)
    {
        bool refused = false;
        try
        {
            ThrownExceptionWatch second;
        }
        catch (const std::logic_error &)
        {
            refused = true;
        }
        if (!refused)
            return Fail("A second watch is refused while one exists.");
        watch.Reset();
        return true;
    }
} // namespace

int TestThrownExceptionWatch()
{
    {
        ThrownExceptionWatch watch;
        const bool passed = CheckQuiet(watch) && CheckOwnThrow(watch) && CheckOtherThreadAndType(watch);
        if (!passed)
            return EXIT_FAILURE;
        if (!CheckRethrows(watch) || !CheckLimitAndReset(watch))
            return EXIT_FAILURE;
        if (!CheckSingleWatch(watch))
            return EXIT_FAILURE;
    }
    // A later watch starts again; throws between watches are not counted by anyone.
    ThrowAndCatch("unwatched");
    ThrownExceptionWatch again;
    if (!CheckQuiet(again))
        return EXIT_FAILURE;
    std::cout << "Thrown-exception watch passed: own throws on any thread are counted with type, what() and stack; "
                 "rethrows count again; records stop at "
              << ThrownExceptionWatch::MaxRecords << "; one watch at a time.\n";
    return EXIT_SUCCESS;
}
