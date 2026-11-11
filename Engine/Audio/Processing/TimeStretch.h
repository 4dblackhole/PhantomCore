#pragma once
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>
// Offline fixed-rate conversion. Output sample zero shares the source origin.
namespace PhantomCore
{
std::vector<float> TimeStretchPcm(std::span<const float> source, std::uint32_t channels,
                                  std::uint32_t sampleRate, double playbackRate, std::stop_token stop);

} // namespace PhantomCore
