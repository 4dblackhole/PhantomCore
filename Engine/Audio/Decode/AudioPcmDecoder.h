#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>

// Interleaved float PCM in source sample frames. Codec priming/padding is
// handled by the decoder; intentional silence remains part of the source.
// The first callback supplies format with an empty span. Spans are borrowed
// only for the callback. Cancellation stops between bounded PCM chunks.
namespace PhantomCore
{
using AudioPcmConsumer = std::function<void(
    std::uint32_t channels, std::uint32_t sampleRate, std::span<const float> samples)>;

// Independent of AudioSystem/device lifetime; suitable for analysis workers.
// Throws std::runtime_error on I/O/codec/format errors. A cancelled read may
// have delivered a prefix; callers must discard it rather than publish it.
void DecodeAudioPcm(const std::filesystem::path& path,
                    std::stop_token stop, const AudioPcmConsumer& consume);

} // namespace PhantomCore
