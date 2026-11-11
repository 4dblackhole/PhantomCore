#include "PcmDecoders.h"
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace PhantomCore
{
namespace
{
    void Check(const HRESULT result)
    {
        if (FAILED(result))
            throw std::runtime_error("PCM decoding failed (Media Foundation): " + std::to_string(result));
    }
    struct MediaRuntime
    {
        MediaRuntime()
        {
            const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (com != RPC_E_CHANGED_MODE)
                Check(com);
            ownsCom = SUCCEEDED(com);
            const auto media = MFStartup(MF_VERSION);
            if (FAILED(media))
            {
                if (ownsCom)
                    CoUninitialize();
                Check(media);
            }
        }
        ~MediaRuntime()
        {
            MFShutdown();
            if (ownsCom)
                CoUninitialize();
        }
        MediaRuntime(const MediaRuntime&) = delete;
        MediaRuntime& operator=(const MediaRuntime&) = delete;
        bool ownsCom{};
    };
    struct LockedBuffer
    {
        explicit LockedBuffer(IMFMediaBuffer& source) : buffer(source)
        {
            Check(buffer.Lock(&data, nullptr, nullptr));
        }
        ~LockedBuffer() { static_cast<void>(buffer.Unlock()); }
        LockedBuffer(const LockedBuffer&) = delete;
        LockedBuffer& operator=(const LockedBuffer&) = delete;
        IMFMediaBuffer& buffer;
        BYTE* data{};
    };
}

void DecodeMediaFoundation(const std::filesystem::path& path, const std::stop_token stop,
                           const AudioPcmConsumer& consume)
{
    MediaRuntime runtime;
    using Microsoft::WRL::ComPtr;
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    ComPtr<IMFSourceReader> reader;
    Check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader));
    Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
    Check(reader->SetStreamSelection(stream, TRUE));
    ComPtr<IMFMediaType> type;
    Check(MFCreateMediaType(&type));
    Check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    Check(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float));
    Check(reader->SetCurrentMediaType(stream, nullptr, type.Get()));
    type.Reset();
    Check(reader->GetCurrentMediaType(stream, &type));
    const auto channels = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    const auto rate = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    if (channels == 0 || channels > 32 || rate == 0)
        throw std::runtime_error("PCM decoding returned an invalid format.");
    consume(channels, rate, {});

    while (!stop.stop_requested())
    {
        DWORD flags{};
        ComPtr<IMFSample> sample;
        Check(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample));
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            throw std::runtime_error("Changing PCM formats are not supported.");
        if (sample)
        {
            ComPtr<IMFMediaBuffer> buffer;
            Check(sample->ConvertToContiguousBuffer(&buffer));
            DWORD bytes{};
            Check(buffer->GetCurrentLength(&bytes));
            if (bytes % (sizeof(float) * channels) != 0)
                throw std::runtime_error("PCM decoding returned an incomplete sample frame.");
            LockedBuffer locked(*buffer.Get());
            auto samples = std::span{reinterpret_cast<const float*>(locked.data), bytes / sizeof(float)};
            while (!samples.empty() && !stop.stop_requested())
            {
                const auto count = std::min(samples.size(), std::size_t{4096} * channels);
                consume(channels, rate, samples.first(count));
                samples = samples.subspan(count);
            }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
}

} // namespace PhantomCore
