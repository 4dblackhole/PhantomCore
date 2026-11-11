#include "AudioPcmDecoder.h"
#include "Submodules/PcmDecoders.h"

#include <array>
#include <fstream>
#include <stdexcept>

namespace PhantomCore
{
void DecodeAudioPcm(const std::filesystem::path& path,
                    const std::stop_token stop, const AudioPcmConsumer& consume)
{
    if (stop.stop_requested())
        return;
    if (!consume)
        throw std::invalid_argument("PCM decoding requires a consumer.");

    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not open the audio file for PCM decoding.");
    std::array<unsigned char, 10> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (input.gcount() < 4)
        throw std::runtime_error("Audio file has an incomplete header.");
    if (header[0] == 'O' && header[1] == 'g' && header[2] == 'g' && header[3] == 'S')
    {
        DecodeVorbis(path, stop, consume);
        return;
    }

    // Probe the codec after ID3 rather than trusting the filename extension.
    // AAC may also have ID3, so a tag alone must not select the MP3 decoder.
    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3' && input.gcount() == 10)
    {
        std::uint32_t bytes{};
        for (std::size_t i = 6; i < 10; ++i)
        {
            if (header[i] & 0x80)
                throw std::runtime_error("Invalid ID3 size in the audio header.");
            bytes = (bytes << 7) | header[i];
        }
        const auto footer = header[3] == 4 && (header[5] & 0x10) ? 10 : 0;
        input.clear();
        input.seekg(static_cast<std::streamoff>(10) + bytes + footer);
        input.read(reinterpret_cast<char*>(header.data()), 4);
    }
    const bool mp3 = input.gcount() >= 4 && header[0] == 0xff &&
                     (header[1] & 0xe0) == 0xe0 && (header[1] & 6) == 2 &&
                     (header[1] & 0x18) != 8 && (header[2] & 0xf0) != 0xf0 &&
                     (header[2] & 0x0c) != 0x0c;
    input.close();
    if (mp3)
        DecodeMp3(path, stop, consume);
    else
        DecodeMediaFoundation(path, stop, consume);
}

} // namespace PhantomCore
