#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>
// Offline fixed-rate conversion. Output sample zero shares the source origin.
namespace PhantomCore
{
// Returns no PCM when stop is requested before the conversion finishes: cancellation is an expected outcome,
// not an error. Invalid parameters throw std::invalid_argument; an output too long for the audio backend throws
// std::runtime_error.
std::optional<std::vector<float>> TimeStretchPcm(std::span<const float> source, std::uint32_t channels,
                                                 std::uint32_t sampleRate, double playbackRate, std::stop_token stop);

} // namespace PhantomCore
