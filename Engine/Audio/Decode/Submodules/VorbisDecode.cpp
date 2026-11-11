#include "PcmDecoders.h"
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace PhantomCore
{
void DecodeVorbis(const std::filesystem::path& path, const std::stop_token stop,
                  const AudioPcmConsumer& consume)
{
    std::FILE* opened{};
    if (_wfopen_s(&opened, path.c_str(), L"rb") != 0 || !opened)
        throw std::runtime_error("Could not open the Ogg Vorbis file for PCM decoding.");
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> file(opened, &std::fclose);
    int error{};
    const std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> decoder(
        stb_vorbis_open_file(file.get(), 0, &error, nullptr), &stb_vorbis_close);
    if (!decoder)
        throw std::runtime_error("Ogg Vorbis PCM decoding could not open the stream (error " +
                                 std::to_string(error) + ").");
    const auto info = stb_vorbis_get_info(decoder.get());
    constexpr std::size_t chunkFrames = 4096;
    if (info.channels <= 0 || info.channels > 32 || info.sample_rate == 0)
        throw std::runtime_error("Ogg Vorbis PCM decoding returned an invalid format.");
    const auto channels = static_cast<std::uint32_t>(info.channels);
    consume(channels, info.sample_rate, {});
    std::vector<float> samples(chunkFrames * channels);
    while (!stop.stop_requested())
    {
        const int frames = stb_vorbis_get_samples_float_interleaved(
            decoder.get(), info.channels, samples.data(), static_cast<int>(samples.size()));
        error = stb_vorbis_get_error(decoder.get());
        if (error != VORBIS__no_error)
            throw std::runtime_error("Ogg Vorbis PCM decoding failed (error " + std::to_string(error) + ").");
        if (frames == 0)
            break;
        if (frames < 0 || static_cast<std::size_t>(frames) > chunkFrames)
            throw std::runtime_error("Ogg Vorbis PCM decoding returned an invalid frame count.");
        consume(channels, info.sample_rate,
                std::span{samples}.first(static_cast<std::size_t>(frames) * channels));
    }
}

} // namespace PhantomCore
