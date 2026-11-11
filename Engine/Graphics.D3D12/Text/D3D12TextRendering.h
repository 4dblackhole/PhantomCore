#pragma once

#include "Text/TextRendering.h"
#include <d3d12.h>
#include <span>

// Keeps native initialization and recording outside the public text contract.
struct TextRenderingBackend final
{
    struct CanvasDraw
    {
        TextLayoutHandle layout;
        TextDrawCommand command;
    };

    static void Initialize(TextRenderSystem& text, ID3D12Device& device,
        DXGI_FORMAT color, DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN);
    static void Shutdown(TextRenderSystem& text) noexcept;
    // A render index can contain multiple off-screen passes. All of them
    // append to the same fence-protected upload arena.
    static void BeginFrame(TextRenderSystem& text, std::uint32_t frameIndex,
        std::uint64_t renderIndex);
    static void Flush(TextRenderSystem& text, ID3D12GraphicsCommandList& commands,
        std::uint32_t width, std::uint32_t height);
    static void DrawCanvasBatch(TextRenderSystem& text,
        ID3D12GraphicsCommandList& commands, std::uint32_t width,
        std::uint32_t height, std::span<const CanvasDraw> draws);
};
