#pragma once

#include <d3dcommon.h>
#include <wrl/client.h>

#include <cstdint>


namespace PhantomCore
{
enum class BuiltInShader : std::uint8_t
{
    UnlitVertexColorInstanced,
    UnlitVertexColorTextureArrayInstanced,
    Text,
    Visual2DRectangle,
};

[[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
    BuiltInShader shader,
    const char* entryPoint,
    const char* target);

} // namespace PhantomCore
