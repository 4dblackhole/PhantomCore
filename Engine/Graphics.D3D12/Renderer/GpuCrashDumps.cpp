#include "Renderer/GpuCrashDumps.h"
#include "Renderer/D3D12Error.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(PHANTOMCORE_AFTERMATH)
// Third-party headers: their warnings are not this engine's to fix.
#pragma warning(push, 0)
#include <GFSDK_Aftermath.h>
#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>
#pragma warning(pop)
#endif

namespace PhantomCore
{
namespace D3D12Detail
{
#if defined(PHANTOMCORE_AFTERMATH)
namespace
{
    constexpr UINT NvidiaVendorId = 0x10DE;
    constexpr wchar_t LibraryName[] = L"GFSDK_Aftermath_Lib.x64.dll";

    // The driver starts its dump when it notices the fault, which may be a
    // moment after the failed call returned. A device that was only removed
    // (no GPU fault) never starts one.
    constexpr auto PollInterval = std::chrono::milliseconds(50);
    constexpr auto MaximumWaitForStart = std::chrono::milliseconds(1500);
    constexpr auto MaximumWaitForDump = std::chrono::milliseconds(8000);

    constexpr std::size_t MarkerCount = static_cast<std::size_t>(GpuCrashMarker::Count);
    // The driver keeps the address of a marker, not a copy; these live for the whole run.
    constexpr std::array<const char*, MarkerCount> MarkerNames{
        "frame: clear",
        "frame: client draws",
        "frame: meshes",
        "frame: visual 2D",
        "frame: text",
        "frame: end",
        "frame: hang test",
        "text: glyph upload",
        "frame: clear color",
        "frame: clear depth",
    };

    // "draw 0" to "draw 1022", and "draw 1023+" for every later draw of a frame.
    constexpr std::size_t DrawMarkerCount = 1024;
    struct DrawMarkerNames final
    {
        std::array<std::array<char, 16>, DrawMarkerCount> text{};
        DrawMarkerNames() noexcept
        {
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                const char* format = index + 1 == text.size() ? "draw %zu+" : "draw %zu";
                static_cast<void>(std::snprintf(text[index].data(), text[index].size(), format, index));
            }
        }
    };
    [[nodiscard]] const DrawMarkerNames& DrawMarkers() noexcept
    {
        static const DrawMarkerNames names;
        return names;
    }

    // The crash dumps whose command list this thread records into.
    thread_local const GpuCrashDumps* attachedDumps = nullptr;

    [[nodiscard]] std::string ResultCode(const GFSDK_Aftermath_Result result)
    {
        return FormatHResult(static_cast<HRESULT>(result));
    }

    [[nodiscard]] const char* Name(const GFSDK_Aftermath_CrashDump_Status status) noexcept
    {
        switch (status)
        {
        case GFSDK_Aftermath_CrashDump_Status_NotStarted:
            return "not started";
        case GFSDK_Aftermath_CrashDump_Status_CollectingData:
            return "still collecting";
        case GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed:
            return "collecting failed";
        case GFSDK_Aftermath_CrashDump_Status_InvokingCallback:
            return "still in the callback";
        case GFSDK_Aftermath_CrashDump_Status_Finished:
            return "finished";
        default:
            return "unknown";
        }
    }

    [[nodiscard]] const char* Name(const GFSDK_Aftermath_Device_Status status) noexcept
    {
        switch (status)
        {
        case GFSDK_Aftermath_Device_Status_Active:
            return "active";
        case GFSDK_Aftermath_Device_Status_Timeout:
            return "timeout";
        case GFSDK_Aftermath_Device_Status_OutOfMemory:
            return "out of memory";
        case GFSDK_Aftermath_Device_Status_PageFault:
            return "page fault";
        case GFSDK_Aftermath_Device_Status_Stopped:
            return "stopped";
        case GFSDK_Aftermath_Device_Status_Reset:
            return "reset";
        case GFSDK_Aftermath_Device_Status_DmaFault:
            return "DMA fault";
        case GFSDK_Aftermath_Device_Status_DeviceRemovedNoGpuFault:
            return "device removed without a GPU fault";
        default:
            return "unknown";
        }
    }

    // The library next to the executable, by its full path: nothing else on
    // the search path can stand in for it.
    [[nodiscard]] HMODULE LoadLibraryBesideExecutable() noexcept
    {
        std::array<wchar_t, 1024> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
            return nullptr;
        std::wstring folder(path.data(), length);
        const std::size_t slash = folder.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return nullptr;
        folder.resize(slash + 1);
        folder += LibraryName;
        return LoadLibraryExW(folder.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    // One exported function, typed by its declaration in the SDK headers.
    template <typename Function>
    [[nodiscard]] bool Find(const HMODULE library, const char* name, Function*& function) noexcept
    {
        function = reinterpret_cast<Function*>(reinterpret_cast<void*>(GetProcAddress(library, name)));
        return function != nullptr;
    }
}

struct GpuCrashDumps::Implementation
{
    HMODULE library{};
    bool enabled{};
    bool markers{};
    GFSDK_Aftermath_ContextHandle context{};

    decltype(GFSDK_Aftermath_EnableGpuCrashDumps)* enable{};
    decltype(GFSDK_Aftermath_DisableGpuCrashDumps)* disable{};
    decltype(GFSDK_Aftermath_GetCrashDumpStatus)* crashDumpStatus{};
    decltype(GFSDK_Aftermath_DX12_Initialize)* initialize{};
    decltype(GFSDK_Aftermath_DX12_CreateContextHandle)* createContext{};
    decltype(GFSDK_Aftermath_ReleaseContextHandle)* releaseContext{};
    decltype(GFSDK_Aftermath_SetEventMarker)* setMarker{};
    decltype(GFSDK_Aftermath_GetDeviceStatus)* deviceStatus{};
    decltype(GFSDK_Aftermath_GpuCrashDump_CreateDecoder)* createDecoder{};
    decltype(GFSDK_Aftermath_GpuCrashDump_DestroyDecoder)* destroyDecoder{};
    decltype(GFSDK_Aftermath_GpuCrashDump_GetDeviceInfo)* decodedDeviceInfo{};
    decltype(GFSDK_Aftermath_GpuCrashDump_GenerateJSON)* generateJson{};
    decltype(GFSDK_Aftermath_GpuCrashDump_GetJSON)* json{};

    // The driver calls back on a thread of its own.
    std::mutex mutex;
    std::vector<std::uint8_t> dump;

    ~Implementation()
    {
        if (context != nullptr && releaseContext != nullptr)
            static_cast<void>(releaseContext(context));
        if (enabled && disable != nullptr)
            static_cast<void>(disable());
        if (library != nullptr)
            FreeLibrary(library);
    }

    [[nodiscard]] bool FindFunctions() noexcept
    {
        bool found = Find(library, "GFSDK_Aftermath_EnableGpuCrashDumps", enable);
        found = Find(library, "GFSDK_Aftermath_DisableGpuCrashDumps", disable) && found;
        found = Find(library, "GFSDK_Aftermath_GetCrashDumpStatus", crashDumpStatus) && found;
        found = Find(library, "GFSDK_Aftermath_DX12_Initialize", initialize) && found;
        found = Find(library, "GFSDK_Aftermath_DX12_CreateContextHandle", createContext) && found;
        found = Find(library, "GFSDK_Aftermath_ReleaseContextHandle", releaseContext) && found;
        found = Find(library, "GFSDK_Aftermath_SetEventMarker", setMarker) && found;
        found = Find(library, "GFSDK_Aftermath_GetDeviceStatus", deviceStatus) && found;
        found = Find(library, "GFSDK_Aftermath_GpuCrashDump_CreateDecoder", createDecoder) && found;
        found = Find(library, "GFSDK_Aftermath_GpuCrashDump_DestroyDecoder", destroyDecoder) && found;
        found = Find(library, "GFSDK_Aftermath_GpuCrashDump_GetDeviceInfo", decodedDeviceInfo) && found;
        found = Find(library, "GFSDK_Aftermath_GpuCrashDump_GenerateJSON", generateJson) && found;
        found = Find(library, "GFSDK_Aftermath_GpuCrashDump_GetJSON", json) && found;
        return found;
    }

    static void GFSDK_AFTERMATH_CALL OnDump(const void* data, const std::uint32_t size, void* user) noexcept
    {
        auto* self = static_cast<Implementation*>(user);
        if (self == nullptr || data == nullptr)
            return;
        try
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            const std::scoped_lock lock(self->mutex);
            self->dump.assign(bytes, bytes + size);
        }
        catch (...)
        {
        }
    }

    // Only what names the program; no paths and nothing about the user.
    static void GFSDK_AFTERMATH_CALL OnDescription(
        const PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription add,
        void*) noexcept
    {
        if (add != nullptr)
            add(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "PhantomCore");
    }

    // A marker is the address of one of MarkerNames; anything else is left as it is.
    static void GFSDK_AFTERMATH_CALL OnResolveMarker(
        const void* marker,
        const std::uint32_t size,
        void*,
        const PFN_GFSDK_Aftermath_ResolveMarker resolve) noexcept
    {
        if (size != 0 || resolve == nullptr)
            return;
        for (const char* name : MarkerNames)
        {
            if (marker == name)
            {
                resolve(name, static_cast<std::uint32_t>(std::strlen(name)));
                return;
            }
        }
        const auto& draws = DrawMarkers().text;
        const auto address = reinterpret_cast<std::uintptr_t>(marker);
        const auto first = reinterpret_cast<std::uintptr_t>(draws.front().data());
        const auto last = reinterpret_cast<std::uintptr_t>(draws.back().data());
        if (address >= first && address <= last)
        {
            const char* name = static_cast<const char*>(marker);
            resolve(name, static_cast<std::uint32_t>(std::strlen(name)));
        }
    }

    [[nodiscard]] GFSDK_Aftermath_CrashDump_Status WaitForDump() const
    {
        GFSDK_Aftermath_CrashDump_Status status = GFSDK_Aftermath_CrashDump_Status_Unknown;
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            if (!GFSDK_Aftermath_SUCCEED(crashDumpStatus(&status)))
                return GFSDK_Aftermath_CrashDump_Status_Unknown;
            const bool done = status == GFSDK_Aftermath_CrashDump_Status_Finished ||
                status == GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed;
            if (done)
                return status;
            const auto waited = std::chrono::steady_clock::now() - start;
            const bool neverStarted =
                status == GFSDK_Aftermath_CrashDump_Status_NotStarted && waited >= MaximumWaitForStart;
            if (neverStarted || waited >= MaximumWaitForDump)
                return status;
            std::this_thread::sleep_for(PollInterval);
        }
    }

    // ", device in the dump: timeout" and the JSON text, when the dump decodes.
    void Decode(GpuCrashDumpResult& result) const
    {
        GFSDK_Aftermath_GpuCrashDump_Decoder decoder{};
        const GFSDK_Aftermath_Result created = createDecoder(
            GFSDK_Aftermath_Version_API,
            result.dump.data(),
            static_cast<std::uint32_t>(result.dump.size()),
            &decoder);
        if (!GFSDK_Aftermath_SUCCEED(created))
        {
            result.status += ", not decoded (" + ResultCode(created) + ")";
            return;
        }

        GFSDK_Aftermath_GpuCrashDump_DeviceInfo device{};
        if (decodedDeviceInfo(decoder, &device) == GFSDK_Aftermath_Result_Success)
            result.status += std::string(", device in the dump: ") + Name(device.status);

        std::uint32_t size = 0;
        const GFSDK_Aftermath_Result generated = generateJson(
            decoder,
            GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO,
            GFSDK_Aftermath_GpuCrashDumpFormatterFlags_NONE,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &size);
        if (generated == GFSDK_Aftermath_Result_Success && size > 0)
        {
            std::string text(size, '\0');
            if (json(decoder, size, text.data()) == GFSDK_Aftermath_Result_Success)
            {
                text.resize(std::strlen(text.c_str()));
                result.json = std::move(text);
            }
        }
        static_cast<void>(destroyDecoder(decoder));
    }
};

GpuCrashDumps::GpuCrashDumps() = default;
GpuCrashDumps::~GpuCrashDumps()
{
    if (attachedDumps == this)
        attachedDumps = nullptr;
}

void GpuCrashDumps::Enable(const UINT adapterVendorId)
{
    if (adapterVendorId != NvidiaVendorId)
    {
        state_ = "off (not an NVIDIA adapter)";
        return;
    }
    auto implementation = std::make_unique<Implementation>();
    implementation->library = LoadLibraryBesideExecutable();
    if (implementation->library == nullptr)
    {
        state_ = "off (GFSDK_Aftermath_Lib.x64.dll not found beside the executable)";
        return;
    }
    if (!implementation->FindFunctions())
    {
        state_ = "off (the library lacks a function this build uses)";
        return;
    }

    const GFSDK_Aftermath_Result result = implementation->enable(
        GFSDK_Aftermath_Version_API,
        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_DX,
        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
        &Implementation::OnDump,
        nullptr,
        &Implementation::OnDescription,
        &Implementation::OnResolveMarker,
        implementation.get());
    if (!GFSDK_Aftermath_SUCCEED(result))
    {
        state_ = "off (enable failed " + ResultCode(result) + ")";
        return;
    }
    implementation->enabled = true;
    implementation_ = std::move(implementation);
    state_ = "on (NVIDIA Aftermath)";
}

bool GpuCrashDumps::Active() const noexcept
{
    return implementation_ != nullptr;
}

const std::string& GpuCrashDumps::State() const noexcept
{
    return state_;
}

void GpuCrashDumps::AttachDevice(ID3D12Device& device)
{
    if (implementation_ == nullptr)
        return;
    // Markers, and the driver's own record of live and recently destroyed
    // resources: a real loss was a GPU page fault, and only that record says
    // which resource the faulting address belonged to and whether it had
    // been destroyed. Call stacks and shader debug data stay off: they cost
    // far more on every call.
    const std::uint32_t features =
        GFSDK_Aftermath_FeatureFlags_EnableMarkers | GFSDK_Aftermath_FeatureFlags_EnableResourceTracking;
    const GFSDK_Aftermath_Result result = implementation_->initialize(
        GFSDK_Aftermath_Version_API,
        features,
        &device);
    if (!GFSDK_Aftermath_SUCCEED(result))
    {
        state_ += ", device not attached " + ResultCode(result);
        return;
    }
    implementation_->markers = true;
}

void GpuCrashDumps::AttachCommandList(ID3D12GraphicsCommandList& commandList)
{
    if (implementation_ == nullptr || !implementation_->markers)
        return;
    const GFSDK_Aftermath_Result result =
        implementation_->createContext(&commandList, &implementation_->context);
    if (!GFSDK_Aftermath_SUCCEED(result))
    {
        implementation_->context = nullptr;
        state_ += ", no frame markers " + ResultCode(result);
        return;
    }
    attachedDumps = this;
}

void GpuCrashDumps::Mark(const GpuCrashMarker marker) const noexcept
{
    if (implementation_ == nullptr || implementation_->context == nullptr)
        return;
    const auto index = static_cast<std::size_t>(marker);
    if (index >= MarkerNames.size())
        return;
    // Size zero: the driver stores the address and asks OnResolveMarker for the text.
    static_cast<void>(implementation_->setMarker(implementation_->context, MarkerNames[index], 0));
}

void GpuCrashDumps::MarkDraw(const std::uint32_t drawNumber) const noexcept
{
    if (implementation_ == nullptr || implementation_->context == nullptr)
        return;
    const auto& names = DrawMarkers().text;
    const std::size_t index = std::min<std::size_t>(drawNumber, names.size() - 1);
    static_cast<void>(implementation_->setMarker(implementation_->context, names[index].data(), 0));
}

void MarkGpuDraw(const std::uint32_t drawNumber) noexcept
{
    if (attachedDumps != nullptr)
        attachedDumps->MarkDraw(drawNumber);
}

void MarkGpuStep(const GpuCrashMarker marker) noexcept
{
    if (attachedDumps != nullptr)
        attachedDumps->Mark(marker);
}

GpuCrashDumpResult GpuCrashDumps::Collect() const
{
    GpuCrashDumpResult result;
    if (implementation_ == nullptr)
    {
        result.status = state_;
        return result;
    }

    const GFSDK_Aftermath_CrashDump_Status status = implementation_->WaitForDump();
    {
        const std::scoped_lock lock(implementation_->mutex);
        result.dump = implementation_->dump;
    }
    result.status = std::string("dump ") + Name(status) + ", " + std::to_string(result.dump.size()) + " bytes";

    GFSDK_Aftermath_Device_Status device = GFSDK_Aftermath_Device_Status_Unknown;
    if (GFSDK_Aftermath_SUCCEED(implementation_->deviceStatus(&device)))
        result.status += std::string(", device now: ") + Name(device);
    if (!result.dump.empty())
        implementation_->Decode(result);
    return result;
}

#else // !PHANTOMCORE_AFTERMATH

struct GpuCrashDumps::Implementation
{
};

GpuCrashDumps::GpuCrashDumps() = default;
GpuCrashDumps::~GpuCrashDumps() = default;

void GpuCrashDumps::Enable(UINT)
{
    state_ = "off (not built: PhantomCoreAftermath)";
}

bool GpuCrashDumps::Active() const noexcept
{
    return false;
}

const std::string& GpuCrashDumps::State() const noexcept
{
    return state_;
}

void GpuCrashDumps::AttachDevice(ID3D12Device&)
{
}

void GpuCrashDumps::AttachCommandList(ID3D12GraphicsCommandList&)
{
}

void GpuCrashDumps::Mark(GpuCrashMarker) const noexcept
{
}

void GpuCrashDumps::MarkDraw(std::uint32_t) const noexcept
{
}

void MarkGpuDraw(std::uint32_t) noexcept
{
}

void MarkGpuStep(GpuCrashMarker) noexcept
{
}

GpuCrashDumpResult GpuCrashDumps::Collect() const
{
    return {state_, {}, {}};
}

#endif

} // namespace D3D12Detail
} // namespace PhantomCore
