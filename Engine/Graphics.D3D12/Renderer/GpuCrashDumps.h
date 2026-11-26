#pragma once

// Renderer feature: GPU crash dumps written by the graphics driver itself
// (NVIDIA Nsight Aftermath). The driver records which shaders and which part
// of the frame the GPU was working on when it stopped, which D3D12's own
// device removed data cannot tell.
//
// The SDK is not part of this repository. A build made with
// PhantomCoreAftermath=true compiles the calls; every other build compiles a
// stand-in that reports "not built". The SDK library is opened at run time
// from the folder of the executable, so a build with the calls still starts
// where the library is missing.

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PhantomCore
{
namespace D3D12Detail
{
    // Where in a frame the GPU is. The driver keeps the last one it reached.
    enum class GpuCrashMarker : std::uint8_t
    {
        FrameClear,
        ClientDraws,
        Meshes,
        Visual2D,
        Text,
        FrameEnd,
        HangTest,
        Count,
    };

    struct GpuCrashDumpResult
    {
        // One line: how far the collection got and what the driver says about the device.
        std::string status;
        // The driver's dump, as NVIDIA's tools read it. Empty when none was made.
        std::vector<std::uint8_t> dump;
        // The same dump decoded to JSON text. Empty when it could not be decoded.
        std::string json;
    };

    class GpuCrashDumps final
    {
    public:
        GpuCrashDumps();
        ~GpuCrashDumps();

        GpuCrashDumps(const GpuCrashDumps&) = delete;
        GpuCrashDumps& operator=(const GpuCrashDumps&) = delete;

        // Before the device is created: the driver only watches devices made
        // afterwards. adapterVendorId is the DXGI vendor of the chosen adapter.
        void Enable(UINT adapterVendorId);
        [[nodiscard]] bool Active() const noexcept;
        // "on" or "off (<reason>)", for the start record.
        [[nodiscard]] const std::string& State() const noexcept;

        // After the device and its frame command list exist. The list must be
        // recording (between Reset and Close).
        void AttachDevice(ID3D12Device& device);
        void AttachCommandList(ID3D12GraphicsCommandList& commandList);
        // On the attached command list, while it is recording.
        void Mark(GpuCrashMarker marker) const noexcept;

        // After a device loss, before the device is released: waits for the
        // driver to finish its dump (a few seconds at most) and returns it.
        [[nodiscard]] GpuCrashDumpResult Collect() const;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
        std::string state_{"off (not requested)"};
    };
}

} // namespace PhantomCore
