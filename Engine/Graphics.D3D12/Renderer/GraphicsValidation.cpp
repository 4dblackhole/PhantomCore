#include "Renderer/GraphicsValidation.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace PhantomCore
{
namespace D3D12Detail
{
namespace
{
    // A message that repeats every frame says nothing new after its first lines.
    constexpr std::uint32_t RecordsPerMessage = 3;
    // Live objects at shutdown: enough to see what leaked, not every page of it.
    constexpr std::uint32_t LiveObjectLines = 40;
    constexpr std::size_t MaximumLineLength = 480;

    // Records are plain ASCII on one line, like the rest of the engine records.
    [[nodiscard]] std::string OneLine(const char* text)
    {
        std::string result;
        for (; text != nullptr && *text != 0 && result.size() < MaximumLineLength; ++text)
        {
            const bool printable = *text >= 32 && *text < 127;
            result.push_back(printable ? *text : ' ');
        }
        return result;
    }

    [[nodiscard]] const char* SeverityName(const D3D12_MESSAGE_SEVERITY severity) noexcept
    {
        switch (severity)
        {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            return "corruption";
        case D3D12_MESSAGE_SEVERITY_ERROR:
            return "error";
        case D3D12_MESSAGE_SEVERITY_WARNING:
            return "warning";
        default:
            return nullptr;
        }
    }

    // Reads the queue's message at index into storage. Null when it cannot.
    [[nodiscard]] const D3D12_MESSAGE* ReadMessage(
        ID3D12InfoQueue& queue,
        const UINT64 index,
        std::vector<std::byte>& storage)
    {
        SIZE_T bytes{};
        if (FAILED(queue.GetMessage(index, nullptr, &bytes)) || bytes == 0)
            return nullptr;
        storage.resize(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        return SUCCEEDED(queue.GetMessage(index, message, &bytes)) ? message : nullptr;
    }

    // Messages are numbered over the queue's whole history: the queue keeps
    // the newest ones and drops the oldest past its count limit.
    [[nodiscard]] UINT64 HistoryLength(ID3D12InfoQueue& queue) noexcept
    {
        return queue.GetNumMessagesDiscardedByMessageCountLimit() + queue.GetNumStoredMessages();
    }
}

ValidationState EnableValidation(const GraphicsValidation requested) noexcept
{
    ValidationState state;
    state.requested = requested;
    GraphicsValidation level = requested;
#if defined(_DEBUG)
    if (level == GraphicsValidation::Off)
        level = GraphicsValidation::DebugLayer;
#endif
    if (level == GraphicsValidation::Off)
        return state;

    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf()))))
    {
        // A Debug build without the layer ran unchecked before too; only a
        // request from the Client is worth a remark.
        state.layerMissing = requested != GraphicsValidation::Off;
        return state;
    }
    debug->EnableDebugLayer();
    state.debugLayer = true;
    if (level != GraphicsValidation::GpuBased)
        return state;

    Microsoft::WRL::ComPtr<ID3D12Debug1> gpuDebug;
    if (SUCCEEDED(debug.As(&gpuDebug)))
    {
        gpuDebug->SetEnableGPUBasedValidation(TRUE);
        state.gpuBased = true;
    }
    return state;
}

std::string DescribeValidation(const ValidationState& state)
{
    if (state.layerMissing)
    {
        return std::string(state.requested == GraphicsValidation::GpuBased ? "debug layer + GPU-based" : "debug layer") +
            " unavailable: D3D12 debug layer not installed (Windows Graphics Tools), running unchecked";
    }
    if (!state.debugLayer)
        return "off";
    if (state.requested == GraphicsValidation::Off)
        return "debug layer (debug build)";
    if (state.gpuBased)
        return "debug layer + GPU-based";
    return state.requested == GraphicsValidation::GpuBased ? "debug layer (GPU-based unavailable)" : "debug layer";
}

void ValidationMessageRelay::Attach(
    ID3D12Device& device,
    IEngineDiagnosticSink* sink,
    const std::uint32_t limit) noexcept
{
    queue_.Reset();
    sink_ = sink;
    limit_ = limit;
    if (sink == nullptr || FAILED(device.QueryInterface(IID_PPV_ARGS(queue_.GetAddressOf()))))
        queue_.Reset();
}

void ValidationMessageRelay::Forward() noexcept
{
    if (queue_ == nullptr)
        return;
    try
    {
        const UINT64 discarded = queue_->GetNumMessagesDiscardedByMessageCountLimit();
        const UINT64 total = discarded + queue_->GetNumStoredMessages();
        // Messages dropped by the queue before they were read are only counted.
        if (next_ < discarded)
        {
            lost_ += discarded - next_;
            next_ = discarded;
        }
        if (next_ > total)
            next_ = total;
        std::vector<std::byte> storage;
        for (; next_ < total; ++next_)
        {
            const D3D12_MESSAGE* message = ReadMessage(*queue_.Get(), next_ - discarded, storage);
            if (message == nullptr)
                ++lost_;
            else
                Record(*message);
        }
    }
    catch (...)
    {
    }
}

void ValidationMessageRelay::Finish(ID3D12Device& device) noexcept
{
    if (queue_ == nullptr)
        return;
    Forward();
    std::uint64_t liveObjects = 0;
    try
    {
        // The device reports its live objects as messages of its own; they are
        // recorded apart from the validation counts.
        Microsoft::WRL::ComPtr<ID3D12DebugDevice> debugDevice;
        if (SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(debugDevice.GetAddressOf()))))
        {
            const UINT64 before = HistoryLength(*queue_.Get());
            debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
            const UINT64 discarded = queue_->GetNumMessagesDiscardedByMessageCountLimit();
            const UINT64 total = HistoryLength(*queue_.Get());
            std::vector<std::byte> storage;
            for (UINT64 index = std::max(before, discarded); index < total; ++index)
            {
                const D3D12_MESSAGE* message = ReadMessage(*queue_.Get(), index - discarded, storage);
                if (message == nullptr)
                    continue;
                if (++liveObjects <= LiveObjectLines)
                    Send("graphics live object: " + OneLine(message->pDescription));
            }
            next_ = total;
        }
        Send("graphics validation summary: " + std::to_string(corruptions_) + " corruptions, " +
            std::to_string(errors_) + " errors, " + std::to_string(warnings_) + " warnings, " +
            std::to_string(recorded_) + " recorded, " + std::to_string(lost_) + " dropped by the queue; " +
            std::to_string(liveObjects) + " live object lines at shutdown");
    }
    catch (...)
    {
    }
    queue_.Reset();
}

void ValidationMessageRelay::Record(const D3D12_MESSAGE& message) noexcept
{
    try
    {
        const char* severity = SeverityName(message.Severity);
        if (severity == nullptr)
            return;
        if (message.Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            ++corruptions_;
        else if (message.Severity == D3D12_MESSAGE_SEVERITY_ERROR)
            ++errors_;
        else
            ++warnings_;

        if (++perMessage_[static_cast<int>(message.ID)] > RecordsPerMessage)
            return;
        if (recorded_ >= limit_)
        {
            if (!limitNoted_)
                Send("graphics validation: " + std::to_string(limit_) + " messages recorded; later ones are only counted");
            limitNoted_ = true;
            return;
        }
        ++recorded_;
        Send(std::string("graphics validation ") + severity + " #" + std::to_string(static_cast<int>(message.ID)) +
            ": " + OneLine(message.pDescription));
    }
    catch (...)
    {
    }
}

void ValidationMessageRelay::Send(const std::string& line) noexcept
{
    if (sink_ != nullptr)
        sink_->OnEngineRecord(line);
}

} // namespace D3D12Detail
} // namespace PhantomCore
