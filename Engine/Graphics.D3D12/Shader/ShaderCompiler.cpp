#include "Shader/ShaderCompiler.h"

#include "Shader/Generated/BuiltInShaders.inl"

#include <d3dcompiler.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#pragma comment(lib, "d3dcompiler.lib")


namespace PhantomCore
{
namespace
{
    struct ShaderSource final
    {
        std::string_view name;
        std::string_view source;
    };

    [[nodiscard]] ShaderSource GetShaderSource(
        const BuiltInShader shader)
    {
        switch (shader)
        {
        case BuiltInShader::UnlitVertexColorInstanced:
            return {
                "UnlitVertexColorInstanced.hlsl",
                UnlitVertexColorInstanced};
        case BuiltInShader::
            UnlitVertexColorTextureArrayInstanced:
            return {
                "UnlitVertexColorTextureArrayInstanced.hlsl",

                    UnlitVertexColorTextureArrayInstanced};
        case BuiltInShader::Text:
            return {
                "Text.hlsl",
                Text};
        case BuiltInShader::Visual2DRectangle:
            return {
                "Visual2DRectangle.hlsl",
                Visual2DRectangle};
        default:
            throw std::invalid_argument(
                "The requested built-in shader is unknown.");
        }
    }
}

Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
    const BuiltInShader shader,
    const char* entryPoint,
    const char* target)
{
    const ShaderSource source = GetShaderSource(shader);
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> byteCode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source.source.data(),
        source.source.size(),
        source.name.data(),
        nullptr,
        nullptr,
        entryPoint,
        target,
        flags,
        0,
        byteCode.ReleaseAndGetAddressOf(),
        errors.ReleaseAndGetAddressOf());

    if (FAILED(result))
    {
        const std::string details = errors != nullptr
            ? std::string(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize())
            : "No compiler diagnostics were returned.";
        throw std::runtime_error(
            "Shader compilation failed for " +
            std::string(source.name) + ":\n" +
            details);
    }

    return byteCode;
}

} // namespace PhantomCore
