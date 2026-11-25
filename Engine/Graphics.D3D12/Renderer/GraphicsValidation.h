#pragma once

// Renderer feature: the D3D12 debug layer, GPU-based validation, and the
// relay that turns their messages into diagnostic records. Nothing here
// changes rendering; it only checks it and reports.

#include "Diagnostics/EngineDiagnostics.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace PhantomCore
{
namespace D3D12Detail
{
    // What could be switched on before the device was created.
    struct ValidationState
    {
        GraphicsValidation requested{};
        bool debugLayer{};
        bool gpuBased{};
        // The debug layer was asked for but the PC has no D3D12 SDK layers
        // (Windows "Graphics Tools" is not installed).
        bool layerMissing{};
    };

    // Turns the debug layer (and GPU-based validation) on for devices created
    // afterwards. Debug builds use at least the debug layer, as they always did.
    // Never installs anything or changes system settings.
    [[nodiscard]] ValidationState EnableValidation(GraphicsValidation requested) noexcept;

    // "off", "debug layer", "debug layer + GPU-based", "debug layer (debug build)",
    // or the requested level with "unavailable: D3D12 debug layer not installed".
    [[nodiscard]] std::string DescribeValidation(const ValidationState& state);

    // Sends the device's debug-layer messages to the diagnostic sink as records,
    // from the thread that renders. Only active when validation was requested;
    // Debug builds that merely have the layer keep their messages to the queue,
    // where RunWithGraphicsDiagnostics and debuggers read them as before.
    class ValidationMessageRelay
    {
    public:
        void Attach(
            ID3D12Device& device,
            IEngineDiagnosticSink* sink,
            std::uint32_t limit) noexcept;
        [[nodiscard]] bool Active() const noexcept
        {
            return queue_ != nullptr;
        }
        // Records the messages stored since the last call, within the limits.
        void Forward() noexcept;
        // Forwards what is still queued, asks the device which objects are
        // still alive and records them, then sends one summary line. Call when
        // everything but the device has been released.
        void Finish(ID3D12Device& device) noexcept;

    private:
        void Record(const D3D12_MESSAGE& message) noexcept;
        void Send(const std::string& line) noexcept;

        Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue_;
        IEngineDiagnosticSink* sink_{};
        std::uint32_t limit_{};
        // Position in the queue's whole history: stored plus discarded messages.
        std::uint64_t next_{};
        std::uint64_t lost_{};
        std::uint64_t corruptions_{};
        std::uint64_t errors_{};
        std::uint64_t warnings_{};
        std::uint32_t recorded_{};
        bool limitNoted_{};
        std::unordered_map<int, std::uint32_t> perMessage_;
    };
}

} // namespace PhantomCore
