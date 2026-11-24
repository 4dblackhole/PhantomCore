#pragma once

// Renderer feature: what the device, its adapter and DRED can say for the
// engine diagnostics. Everything here only reads; nothing changes rendering.

#include "Diagnostics/EngineDiagnostics.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <string>
#include <vector>

namespace PhantomCore
{
namespace D3D12Detail
{
    // Turns on DRED breadcrumbs and page-fault tracking for devices created
    // afterwards. False when this Windows version has no DRED.
    [[nodiscard]] bool EnableDeviceRemovedExtendedData() noexcept;

    // "NVIDIA GeForce ..., vendor 10DE device 1C02, 3072 MB dedicated, driver 32.0.15.6094".
    [[nodiscard]] std::string DescribeAdapter(IDXGIAdapter1& adapter);

    // Unavailable (all zero) when the adapter cannot be asked.
    [[nodiscard]] GraphicsMemoryInfo QueryGraphicsMemory(IDXGIAdapter3* adapter) noexcept;

    // "DXGI_ERROR_DEVICE_HUNG" for the known reasons, the number otherwise.
    [[nodiscard]] std::string DeviceRemovedReasonName(HRESULT reason);

    // One line per command list and per allocation around a page fault. A
    // part that has nothing says so in a line of its own, so the result is
    // never empty.
    [[nodiscard]] std::vector<std::string> ReadDeviceRemovedExtendedData(ID3D12Device& device);
}

} // namespace PhantomCore
