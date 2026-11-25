#include "System/ThrownExceptionWatch.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace PhantomCore
{
namespace
{
    // The code MSVC raises for every C++ throw.
    constexpr DWORD CppExceptionCode = 0xE06D7363;

    // MSVC x64 throw metadata. Every offset is relative to the throwing image's base, which the exception
    // record carries in ExceptionInformation[3] next to the object [1] and this description [2].
    struct ThrowCatchableType
    {
        unsigned properties;
        int typeDescriptor;
        int memberDisplacement, vbtableDisplacement, vdisplacement;
        int size;
        int copyFunction;
    };
    struct ThrowCatchableTypeArray
    {
        int count;
        int types[1];
    };
    struct ThrowDescription
    {
        unsigned attributes;
        int unwind, forwardCompatibility, catchableTypeArray;
    };

    // The C++ runtime libraries throw on behalf of the code that calls them (std::filesystem errors, for one).
    constexpr const wchar_t* RuntimeLibraries[] = {
        L"msvcp140.dll",     L"msvcp140d.dll",     L"msvcp140_1.dll",     L"msvcp140_1d.dll",
        L"msvcp140_2.dll",   L"msvcp140_2d.dll",   L"vcruntime140.dll",   L"vcruntime140d.dll",
        L"vcruntime140_1.dll", L"vcruntime140_1d.dll"};

    // Static storage: the handler reads and writes it without allocating or locking.
    struct WatchState
    {
        std::atomic<bool> owned{};
        std::atomic<bool> counting{};
        std::array<ULONG_PTR, 1 + std::size(RuntimeLibraries)> images{};
        std::size_t imageCount{};
        std::atomic<std::uint64_t> count{};
        std::array<ThrownExceptionRecord, ThrownExceptionWatch::MaxRecords> records{};
    };
    WatchState State;

    // Cuts the text to fit and ends it with a null; no allocation.
    template <std::size_t Size>
    void CopyText(std::array<char, Size>& target, const char* text) noexcept
    {
        if (text == nullptr)
        {
            target[0] = '\0';
            return;
        }
        strncpy_s(target.data(), target.size(), text, _TRUNCATE);
    }

    [[nodiscard]] bool CountedImage(const ULONG_PTR image) noexcept
    {
        for (std::size_t i = 0; i < State.imageCount; ++i)
        {
            if (State.images[i] == image)
                return true;
        }
        return false;
    }

    // The thrown type's name and, for std::exception types, what(). A bare `throw;` carries no description.
    void Describe(const EXCEPTION_RECORD& record, ThrownExceptionRecord& out) noexcept
    {
        const auto base = record.ExceptionInformation[3];
        const auto* description = reinterpret_cast<const ThrowDescription*>(record.ExceptionInformation[2]);
        const auto* object = reinterpret_cast<const char*>(record.ExceptionInformation[1]);
        if (base == 0 || description == nullptr || description->catchableTypeArray == 0)
            return;
        const auto* types = reinterpret_cast<const ThrowCatchableTypeArray*>(base + description->catchableTypeArray);
        for (int i = 0; i < types->count; ++i)
        {
            const auto* type = reinterpret_cast<const ThrowCatchableType*>(base + types->types[i]);
            // A type descriptor is a vtable pointer, a spare pointer, then the decorated name.
            const auto* name = reinterpret_cast<const char*>(base + type->typeDescriptor) + 2 * sizeof(void*);
            if (i == 0)
                CopyText(out.typeName, name);
            if (object == nullptr || std::string_view(name) != ".?AVexception@std@@")
                continue;
            CopyText(out.what, reinterpret_cast<const std::exception*>(object + type->memberDisplacement)->what());
            return;
        }
    }

    LONG CALLBACK CountThrow(EXCEPTION_POINTERS* pointers) noexcept
    {
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        const bool cppThrow = record.ExceptionCode == CppExceptionCode && record.NumberParameters >= 4;
        if (!cppThrow || !State.counting.load(std::memory_order_acquire))
            return EXCEPTION_CONTINUE_SEARCH;
        if (!CountedImage(record.ExceptionInformation[3]))
            return EXCEPTION_CONTINUE_SEARCH;
        const auto index = State.count.fetch_add(1, std::memory_order_relaxed);
        if (index < ThrownExceptionWatch::MaxRecords)
        {
            ThrownExceptionRecord& out = State.records[index];
            out.threadId = GetCurrentThreadId();
            out.frameCount = RtlCaptureStackBackTrace(0, static_cast<DWORD>(out.frames.size()), out.frames.data(), nullptr);
            Describe(record, out);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // This executable (it contains the engine) and the runtime libraries it has loaded.
    void FindCountedImages() noexcept
    {
        State.imageCount = 0;
        HMODULE module = nullptr;
        const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&CountThrow), &module))
            State.images[State.imageCount++] = reinterpret_cast<ULONG_PTR>(module);
        for (const auto* name : RuntimeLibraries)
        {
            if (const HMODULE library = GetModuleHandleW(name))
                State.images[State.imageCount++] = reinterpret_cast<ULONG_PTR>(library);
        }
    }
} // namespace

ThrownExceptionWatch::ThrownExceptionWatch()
{
    bool expected = false;
    if (!State.owned.compare_exchange_strong(expected, true))
        throw std::logic_error("Only one ThrownExceptionWatch can exist at a time.");
    FindCountedImages();
    Reset();
    // Last in the chain: every other handler sees an exception first, as without the watch.
    handler_ = AddVectoredExceptionHandler(0, CountThrow);
    if (handler_ == nullptr)
    {
        State.owned.store(false);
        throw std::runtime_error("The thrown-exception watch could not add its exception handler.");
    }
    State.counting.store(true, std::memory_order_release);
}

ThrownExceptionWatch::~ThrownExceptionWatch()
{
    State.counting.store(false, std::memory_order_release);
    RemoveVectoredExceptionHandler(handler_);
    State.owned.store(false);
}

std::uint64_t ThrownExceptionWatch::Count() const noexcept
{
    return State.count.load(std::memory_order_acquire);
}

std::span<const ThrownExceptionRecord> ThrownExceptionWatch::Records() const noexcept
{
    const auto count = Count();
    const auto kept = count < MaxRecords ? static_cast<std::size_t>(count) : MaxRecords;
    return {State.records.data(), kept};
}

void ThrownExceptionWatch::Reset() noexcept
{
    State.count.store(0, std::memory_order_release);
    State.records = {};
}

} // namespace PhantomCore
