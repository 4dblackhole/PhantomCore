#include "PcmDecoders.h"
#include <dr_mp3.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace PhantomCore
{
void DecodeMp3(const std::filesystem::path& path, const std::stop_token stop,
               const AudioPcmConsumer& consume)
{
    // The codec state is larger than the worker's FFT/decoder stack budget.
    auto state = std::make_unique<drmp3>();
    if (!drmp3_init_file_w(state.get(), path.c_str(), nullptr))
        throw std::runtime_error("MP3 PCM decoding could not open the stream.");
    const auto release = [](drmp3* decoder) { drmp3_uninit(decoder); delete decoder; };
    const std::unique_ptr<drmp3, decltype(release)> decoder(state.release(), release);
    if (decoder->channels == 0 || decoder->channels > 2 || decoder->sampleRate == 0)
        throw std::runtime_error("MP3 PCM decoding returned an invalid format.");

    consume(decoder->channels, decoder->sampleRate, {});
    constexpr std::size_t chunkFrames = 4096;
    std::vector<float> samples(chunkFrames * decoder->channels);
    while (!stop.stop_requested())
    {
        // dr_mp3 excludes the Xing/Info frame and LAME priming/end padding.
        const auto frames = drmp3_read_pcm_frames_f32(decoder.get(), chunkFrames, samples.data());
        if (frames == 0)
            break;
        if (frames > chunkFrames)
            throw std::runtime_error("MP3 PCM decoding returned an invalid frame count.");
        consume(decoder->channels, decoder->sampleRate,
                std::span{samples}.first(static_cast<std::size_t>(frames) * decoder->channels));
    }
}

} // namespace PhantomCore
