#include "Renderer/GpuHangTest.h"
#include "Renderer/D3D12Error.h"

#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace PhantomCore
{
namespace D3D12Detail
{
namespace
{
    // The loop advances by Step, which the draw sets to zero. The compiler
    // cannot know that, so it keeps the loop.
    constexpr char Source[] = R"(
cbuffer Hang : register(b0)
{
    uint Step;
};

float4 VertexMain(uint vertex : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * 2.0F - 1.0F, 0.0F, 1.0F);
}

float4 PixelMain(float4 position : SV_Position) : SV_Target
{
    float value = position.x;
    [loop]
    for (uint index = 0; index != 0xFFFFFFFF; index += Step)
    {
        value = sin(value) + position.y;
    }
    return float4(value, 0.0F, 0.0F, 1.0F);
}
)";

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* entryPoint, const char* target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> byteCode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            Source,
            std::strlen(Source),
            "GpuHangTest",
            nullptr,
            nullptr,
            entryPoint,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            byteCode.ReleaseAndGetAddressOf(),
            errors.ReleaseAndGetAddressOf());
        if (FAILED(result))
        {
            std::string message = "The GPU hang test shader did not compile";
            if (errors != nullptr)
                message += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
            throw std::runtime_error(message);
        }
        return byteCode;
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device& device)
    {
        D3D12_ROOT_PARAMETER step{};
        step.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        step.Constants.ShaderRegister = 0;
        step.Constants.Num32BitValues = 1;
        step.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 1;
        description.pParameters = &step;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        ThrowIfFailed(
            D3D12SerializeRootSignature(
                &description,
                D3D_ROOT_SIGNATURE_VERSION_1,
                serialized.GetAddressOf(),
                errors.GetAddressOf()),
            "D3D12SerializeRootSignature");
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        ThrowIfFailed(
            device.CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(rootSignature.GetAddressOf())),
            "ID3D12Device::CreateRootSignature");
        return rootSignature;
    }
}

GpuHangObjects RecordGpuHang(
    ID3D12Device& device,
    ID3D12GraphicsCommandList& commandList,
    const DXGI_FORMAT renderTargetFormat,
    const DXGI_FORMAT depthStencilFormat)
{
    GpuHangObjects objects;
    objects.rootSignature = CreateRootSignature(device);
    const auto vertexShader = Compile("VertexMain", "vs_5_0");
    const auto pixelShader = Compile("PixelMain", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = objects.rootSignature.Get();
    description.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    description.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = renderTargetFormat;
    description.DSVFormat = depthStencilFormat;
    description.SampleDesc.Count = 1;
    ThrowIfFailed(
        device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(objects.pipeline.GetAddressOf())),
        "ID3D12Device::CreateGraphicsPipelineState");

    commandList.SetGraphicsRootSignature(objects.rootSignature.Get());
    commandList.SetPipelineState(objects.pipeline.Get());
    commandList.SetGraphicsRoot32BitConstant(0, 0, 0);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.DrawInstanced(3, 1, 0, 0);
    return objects;
}

} // namespace D3D12Detail
} // namespace PhantomCore
