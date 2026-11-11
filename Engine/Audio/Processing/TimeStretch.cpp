#include "TimeStretch.h"
#include <signalsmith-stretch/signalsmith-stretch.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace PhantomCore
{
namespace
{
    struct Input
    {
        std::span<const float> source;
        std::size_t channels, offset;
        struct Channel
        {
            const Input &input; std::size_t channel;
            float operator[](int index) const
            {
                const auto frame = input.offset + static_cast<std::size_t>(index);
                return frame < input.source.size() / input.channels ? input.source[frame * input.channels + channel] : 0.0F;
            }
        };
        Channel operator[](int channel) const { return {*this, static_cast<std::size_t>(channel)}; }
    };
    struct Output
    {
        float *samples; std::size_t channels;
        struct Channel
        {
            float *samples; std::size_t stride;
            float &operator[](int index) const { return samples[static_cast<std::size_t>(index) * stride]; }
        };
        Channel operator[](int channel) const { return {samples + channel, channels}; }
    };
}
std::vector<float> TimeStretchPcm(const std::span<const float> source, const std::uint32_t channels,
                                  const std::uint32_t sampleRate, const double rate, const std::stop_token stop)
{
    if (!channels || channels > 32 || !sampleRate || source.empty() || source.size() % channels ||
        !std::isfinite(rate) || rate < 0.5 || rate > 2.0)
        throw std::invalid_argument("Invalid PCM time-stretch parameters.");
    if (stop.stop_requested()) throw std::runtime_error("PCM time-stretch cancelled.");
    if (rate == 1.0) return {source.begin(), source.end()};
    const auto frames = source.size() / channels;
    const double outputFrames = std::round(static_cast<double>(frames) / rate);
    constexpr auto maximumSamples = std::numeric_limits<unsigned int>::max() / sizeof(float);
    if (outputFrames < 1 || outputFrames > maximumSamples / channels)
        throw std::runtime_error("Time-stretched PCM exceeds the backend byte-length range.");
    const auto count = static_cast<std::size_t>(outputFrames);
    signalsmith::stretch::SignalsmithStretch<float> stretch(0);
    stretch.presetDefault(static_cast<int>(channels), static_cast<float>(sampleRate));
    // Prime source lookahead, then remove precisely the documented output
    // latency. Intentional source silence is preserved; no energy-based trim.
    const auto lookahead = static_cast<std::size_t>(stretch.inputLatency());
    const auto latency = static_cast<std::size_t>(stretch.outputLatency());
    stretch.seek(Input{source, channels, 0}, static_cast<int>(lookahead), rate);
    std::vector<float> delayed((count + latency) * channels);
    std::size_t inputPosition = 0, outputPosition = 0;
    while (inputPosition < frames)
    {
        if (stop.stop_requested()) throw std::runtime_error("PCM time-stretch cancelled.");
        const auto nextInput = std::min(inputPosition + 512, frames);
        const auto nextOutput = nextInput == frames ? count : static_cast<std::size_t>(std::llround(nextInput / rate));
        stretch.process(Input{source, channels, lookahead + inputPosition}, static_cast<int>(nextInput - inputPosition),
                        Output{delayed.data() + outputPosition * channels, channels}, static_cast<int>(nextOutput - outputPosition));
        inputPosition = nextInput;
        outputPosition = nextOutput;
    }
    stretch.flush(Output{delayed.data() + count * channels, channels}, static_cast<int>(latency), static_cast<float>(rate));
    std::vector<float> result(delayed.begin() + static_cast<std::ptrdiff_t>(latency * channels), delayed.end());
    return result;
}

} // namespace PhantomCore
