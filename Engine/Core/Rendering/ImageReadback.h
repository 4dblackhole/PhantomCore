#pragma once

#include <cstdint>
#include <vector>

// CPU-owned image of a completed frame. Pixels are tightly packed RGBA bytes,
// four bytes per pixel, with rows running from the top of the image to the bottom.
// Channels contain the rendered output values without an additional color transform.
struct RgbaImage final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels;
};
