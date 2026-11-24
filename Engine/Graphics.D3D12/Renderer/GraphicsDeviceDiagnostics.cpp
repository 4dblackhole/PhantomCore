#include "Renderer/GraphicsDeviceDiagnostics.h"
#include "Renderer/D3D12Error.h"

#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace PhantomCore
{
namespace D3D12Detail
{
namespace
{
    constexpr std::uint64_t BytesPerMegabyte = 1024ULL * 1024ULL;
    // A report is read by a person. Lists that ran to completion and long
    // allocation lists add nothing after the first few dozen lines.
    constexpr std::size_t MaximumAllocationLines = 24;
    constexpr UINT OperationsShownAroundStop = 6;

    // Records are plain ASCII: driver and debug names are, and a character
    // outside it is replaced so the Client can write lines without a codec.
    [[nodiscard]] std::string Narrow(const wchar_t* text)
    {
        std::string result;
        for (; text != nullptr && *text != 0; ++text)
        {
            const bool printable = *text >= 32 && *text < 127;
            result.push_back(printable ? static_cast<char>(*text) : '?');
        }
        return result.empty() ? "(unnamed)" : result;
    }

    [[nodiscard]] std::string FourHexDigits(const UINT value)
    {
        return FormatHResult(static_cast<HRESULT>(value)).substr(6);
    }

    [[nodiscard]] const char* OperationName(const D3D12_AUTO_BREADCRUMB_OP operation) noexcept
    {
        switch (operation)
        {
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:
            return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
            return "DrawIndexedInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:
            return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:
            return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:
            return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:
            return "ClearRenderTargetView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:
            return "ClearDepthStencilView";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
            return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT:
            return "Present";
        default:
            return nullptr;
        }
    }

    [[nodiscard]] std::string DescribeOperation(const D3D12_AUTO_BREADCRUMB_OP operation)
    {
        const char* name = OperationName(operation);
        return name != nullptr ? name : "operation " + std::to_string(static_cast<int>(operation));
    }

    // "command list Frame on queue Main: completed 41 of 57 operations; ... DrawInstanced >> DrawInstanced ..."
    // The operation after ">>" is the first one the GPU did not finish.
    [[nodiscard]] std::string DescribeCommandList(const D3D12_AUTO_BREADCRUMB_NODE& node)
    {
        const UINT completed = node.pLastBreadcrumbValue != nullptr ? *node.pLastBreadcrumbValue : 0;
        std::string line = "command list " + Narrow(node.pCommandListDebugNameW) + " on queue " +
            Narrow(node.pCommandQueueDebugNameW) + ": completed " + std::to_string(completed) + " of " +
            std::to_string(node.BreadcrumbCount) + " operations";
        if (completed >= node.BreadcrumbCount || node.pCommandHistory == nullptr)
            return line;

        const UINT first = completed > OperationsShownAroundStop ? completed - OperationsShownAroundStop : 0;
        const UINT last = std::min(node.BreadcrumbCount, completed + OperationsShownAroundStop);
        line += ";";
        for (UINT index = first; index < last; ++index)
        {
            line += index == completed ? " >> " : " ";
            line += DescribeOperation(node.pCommandHistory[index]);
        }
        return line;
    }

    void AppendAllocations(
        const D3D12_DRED_ALLOCATION_NODE* node,
        const char* state,
        std::vector<std::string>& lines)
    {
        for (; node != nullptr && lines.size() < MaximumAllocationLines; node = node->pNext)
        {
            lines.push_back(std::string(state) + " allocation " + Narrow(node->ObjectNameW) + " (type " +
                std::to_string(static_cast<int>(node->AllocationType)) + ")");
        }
    }
}

bool EnableDeviceRemovedExtendedData() noexcept
{
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(settings.GetAddressOf()))))
        return false;
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    return true;
}

std::string DescribeAdapter(IDXGIAdapter1& adapter)
{
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter.GetDesc1(&description)))
        return "unknown adapter";

    std::string line = Narrow(description.Description) + ", vendor " + FourHexDigits(description.VendorId) +
        " device " + FourHexDigits(description.DeviceId) + ", " +
        std::to_string(description.DedicatedVideoMemory / BytesPerMegabyte) + " MB dedicated";
    // The user-mode driver version, as Device Manager shows it.
    LARGE_INTEGER version{};
    if (SUCCEEDED(adapter.CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
    {
        const auto bits = static_cast<std::uint64_t>(version.QuadPart);
        line += ", driver " + std::to_string((bits >> 48) & 0xFFFF) + "." + std::to_string((bits >> 32) & 0xFFFF) +
            "." + std::to_string((bits >> 16) & 0xFFFF) + "." + std::to_string(bits & 0xFFFF);
    }
    return line;
}

GraphicsMemoryInfo QueryGraphicsMemory(IDXGIAdapter3* adapter) noexcept
{
    if (adapter == nullptr)
        return {};
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO shared{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
        return {};
    // The shared figure is extra detail; the report is still useful without it.
    static_cast<void>(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared));
    return {true, local.CurrentUsage, local.Budget, shared.CurrentUsage};
}

std::string DeviceRemovedReasonName(const HRESULT reason)
{
    switch (reason)
    {
    case S_OK:
        return "S_OK";
    case DXGI_ERROR_DEVICE_HUNG:
        return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED:
        return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET:
        return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL:
        return "DXGI_ERROR_INVALID_CALL";
    default:
        return FormatHResult(reason);
    }
}

std::vector<std::string> ReadDeviceRemovedExtendedData(ID3D12Device& device)
{
    std::vector<std::string> lines;
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> data;
    if (FAILED(device.QueryInterface(IID_PPV_ARGS(data.GetAddressOf()))))
    {
        lines.emplace_back("the device has no device removed data interface");
        return lines;
    }

    // Each part says so when it has nothing, so an empty answer is not
    // mistaken for data that was never asked for.
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    const HRESULT breadcrumbResult = data->GetAutoBreadcrumbsOutput(&breadcrumbs);
    if (FAILED(breadcrumbResult))
        lines.push_back("command list history unavailable: " + FormatHResult(breadcrumbResult));
    else if (breadcrumbs.pHeadAutoBreadcrumbNode == nullptr)
        lines.emplace_back("no command list history was kept");
    else
    {
        for (const auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext)
            lines.push_back(DescribeCommandList(*node));
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    const HRESULT faultResult = data->GetPageFaultAllocationOutput(&fault);
    if (FAILED(faultResult))
    {
        lines.push_back("page fault data unavailable: " + FormatHResult(faultResult));
        return lines;
    }
    if (fault.PageFaultVA == 0)
    {
        lines.emplace_back("no page fault");
        return lines;
    }
    lines.push_back("page fault at GPU address " + std::to_string(fault.PageFaultVA));
    std::vector<std::string> allocations;
    AppendAllocations(fault.pHeadExistingAllocationNode, "existing", allocations);
    AppendAllocations(fault.pHeadRecentFreedAllocationNode, "recently freed", allocations);
    lines.insert(lines.end(), allocations.begin(), allocations.end());
    return lines;
}

} // namespace D3D12Detail
} // namespace PhantomCore
