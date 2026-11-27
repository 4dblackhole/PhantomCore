#include "Renderer/FrameDrawRecord.h"

#include <algorithm>
#include <cstring>

namespace PhantomCore
{
namespace D3D12Detail
{
namespace
{
    // Enough for every instance of an ordinary draw; a huge batch keeps its first part.
    constexpr std::size_t MaximumBytesPerDraw = 64ULL * 1024ULL;
    constexpr std::size_t MaximumBytesPerFrame = 4ULL * 1024ULL * 1024ULL;
    constexpr std::size_t MaximumNotesPerFrame = 512;

    [[nodiscard]] const char* Name(const RecordedDrawKind kind) noexcept
    {
        switch (kind)
        {
        case RecordedDrawKind::Rectangles:
            return "rectangles";
        case RecordedDrawKind::Images:
            return "images";
        case RecordedDrawKind::Text:
            return "text";
        default:
            return "meshes";
        }
    }

    [[nodiscard]] std::string Hex(const std::uint64_t value)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string text = "0x0000000000000000";
        for (std::size_t index = 0; index < 16; ++index)
            text[index + 2] = digits[(value >> ((15 - index) * 4)) & 0xF];
        return text;
    }
}

FrameDrawRecorder& FrameDrawRecorder::Instance() noexcept
{
    thread_local FrameDrawRecorder recorder;
    return recorder;
}

void FrameDrawRecorder::Enable(const bool enabled) noexcept
{
    enabled_ = enabled;
    for (Frame& frame : frames_)
    {
        frame.begun = false;
        frame.draws.clear();
        frame.data.clear();
        frame.notes.clear();
    }
}

void FrameDrawRecorder::BeginFrame(const std::uint32_t frameIndex, const std::uint64_t renderIndex) noexcept
{
    if (!enabled_)
        return;
    current_ = (current_ + 1) % static_cast<std::uint32_t>(frames_.size());
    Frame& frame = frames_[current_];
    frame.renderIndex = renderIndex;
    frame.slot = frameIndex;
    frame.backBuffer = 0;
    frame.width = 0;
    frame.height = 0;
    frame.finishedFence = 0;
    frame.fence = 0;
    frame.begun = true;
    frame.draws.clear();
    frame.data.clear();
    frame.notes.clear();
}

void FrameDrawRecorder::DescribeTarget(const std::uint32_t backBuffer, const std::uint32_t width,
    const std::uint32_t height, const std::uint64_t finishedFence) noexcept
{
    Frame& frame = frames_[current_];
    frame.backBuffer = backBuffer;
    frame.width = width;
    frame.height = height;
    frame.finishedFence = finishedFence;
}

void FrameDrawRecorder::EndFrame(const std::uint64_t fenceValue) noexcept
{
    frames_[current_].fence = fenceValue;
}

void FrameDrawRecorder::Add(const RecordedDraw& draw) noexcept
{
    Frame& frame = frames_[current_];
    if (!frame.begun)
        return;
    try
    {
        StoredDraw stored{draw, frame.data.size(), 0};
        stored.draw.instanceData = nullptr;
        const std::size_t wanted = static_cast<std::size_t>(draw.instances) * draw.instanceBytes;
        const std::size_t room = MaximumBytesPerFrame - std::min(MaximumBytesPerFrame, frame.data.size());
        const std::size_t bytes = std::min({wanted, MaximumBytesPerDraw, room});
        if (draw.instanceData != nullptr && bytes != 0)
        {
            frame.data.resize(frame.data.size() + bytes);
            std::memcpy(frame.data.data() + stored.dataOffset, draw.instanceData, bytes);
            stored.dataBytes = bytes;
        }
        frame.draws.push_back(stored);
    }
    catch (...)
    {
        // The record is a diagnostic: a failed allocation drops this draw from it and nothing else.
    }
}

void FrameDrawRecorder::Note(const char* what, const std::uint64_t first, const std::uint64_t second) noexcept
{
    Frame& frame = frames_[current_];
    if (!frame.begun || frame.notes.size() >= MaximumNotesPerFrame)
        return;
    try
    {
        frame.notes.push_back({what, first, second, frame.draws.size()});
    }
    catch (...)
    {
        // As for a draw: the record loses this note and nothing else.
    }
}

void FrameDrawRecorder::Describe(std::vector<std::string>& lines, std::vector<std::uint8_t>& data) const
{
    if (!enabled_)
        return;
    std::array<const Frame*, RecordedFrames> ordered{};
    for (std::size_t index = 0; index < frames_.size(); ++index)
        ordered[index] = &frames_[index];
    std::ranges::sort(ordered, {}, [](const Frame* frame) { return frame->renderIndex; });

    for (const Frame* frame : ordered)
    {
        if (!frame->begun)
            continue;
        lines.push_back("frame " + std::to_string(frame->renderIndex) + " (ring slot " + std::to_string(frame->slot) +
            ", back buffer " + std::to_string(frame->backBuffer) + " " + std::to_string(frame->width) + "x" +
            std::to_string(frame->height) + ", fence " +
            (frame->fence != 0 ? std::to_string(frame->fence) : std::string("not submitted")) +
            ", the GPU had finished fence " + std::to_string(frame->finishedFence) + " when it began): " +
            std::to_string(frame->draws.size()) + " draws");
        const std::size_t base = data.size();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(frame->data.data());
        data.insert(data.end(), bytes, bytes + frame->data.size());
        std::size_t number = 0;
        std::size_t nextNote = 0;
        const auto describeNotes = [&lines, frame, &nextNote](const std::size_t beforeDraw)
        {
            while (nextNote < frame->notes.size() && frame->notes[nextNote].beforeDraw <= beforeDraw)
            {
                const StoredNote& note = frame->notes[nextNote++];
                lines.push_back(std::string("  note: ") + note.what + ": " + std::to_string(note.first) + ", " +
                    Hex(note.second));
            }
        };
        for (const StoredDraw& stored : frame->draws)
        {
            describeNotes(number);
            const RecordedDraw& draw = stored.draw;
            std::string line = "  #" + std::to_string(number++) + " " + Name(draw.kind) + " x" +
                std::to_string(draw.instances);
            if (draw.targetWidth != 0)
            {
                line += std::string(draw.screenTarget ? " screen " : " canvas ") + std::to_string(draw.targetWidth) +
                    "x" + std::to_string(draw.targetHeight);
            }
            line += ", instances at " + Hex(draw.bufferAddress) + " (" + std::to_string(draw.instanceBytes) +
                " bytes each, buffer " + std::to_string(draw.bufferBytes) + " bytes)";
            if (draw.textureDescriptors != 0)
                line += ", textures " + Hex(draw.textureDescriptors);
            line += ", data @" + std::to_string(base + stored.dataOffset) + " +" + std::to_string(stored.dataBytes);
            lines.push_back(std::move(line));
        }
        describeNotes(frame->draws.size());
    }
}

} // namespace D3D12Detail
} // namespace PhantomCore
