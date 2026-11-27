#pragma once

// Renderer feature: what the last frames drew, kept for the report of a lost
// graphics device. A GPU page fault says only that some draw read memory that
// was not there; this record says which draws the frame had, which buffer each
// one read its instances from, and what the CPU had written there.
//
// It is off unless a run asks for GPU crash dumps, and then costs one small
// copy per draw. Recording happens on the render thread only.

#include "Renderer/FrameResources.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PhantomCore
{
namespace D3D12Detail
{
    enum class RecordedDrawKind : std::uint8_t
    {
        Rectangles,
        Images,
        Text,
        Meshes,
    };

    struct RecordedDraw
    {
        RecordedDrawKind kind{};
        // The back buffer, or a canvas rendered into a texture.
        bool screenTarget{};
        std::uint32_t targetWidth{};
        std::uint32_t targetHeight{};
        std::uint32_t instances{};
        std::uint32_t instanceBytes{};
        // Where the vertex shader reads the instances: the GPU address of the
        // first one and the size of the whole buffer it lies in.
        std::uint64_t bufferAddress{};
        std::uint64_t bufferBytes{};
        // The first descriptor of the textures the draw may sample; zero when it samples none.
        std::uint64_t textureDescriptors{};
        // What the CPU wrote for these instances; copied when the draw is recorded. May be null.
        const std::byte* instanceData{};
    };

    // The frame ring holds two frames; the record keeps more, so the notes show what led up to them.
    inline constexpr std::size_t RecordedFrames = 8;

    class FrameDrawRecorder final
    {
    public:
        // The one recorder of this thread's renderer.
        [[nodiscard]] static FrameDrawRecorder& Instance() noexcept;

        void Enable(bool enabled) noexcept;
        [[nodiscard]] bool Enabled() const noexcept { return enabled_; }

        // frameIndex is the frame ring slot: its buffers are the ones the frame's draws read.
        void BeginFrame(std::uint32_t frameIndex, std::uint64_t renderIndex) noexcept;
        // What the frame draws into, and how far the GPU had come when the frame began: the last frame
        // fence value it had finished. A frame whose own fence is above that was not finished then.
        void DescribeTarget(std::uint32_t backBuffer, std::uint32_t width, std::uint32_t height,
            std::uint64_t finishedFence) noexcept;
        // The fence value signalled after this frame; zero until the frame was submitted.
        void EndFrame(std::uint64_t fenceValue) noexcept;
        void Add(const RecordedDraw& draw) noexcept;
        // Something that happened to a GPU resource (created, grown, uploaded, retired). It is kept with
        // the frame begun last and with the draw it came before; one after the last draw happened after
        // that frame was recorded and before the next began. what must be a string literal.
        void Note(const char* what, std::uint64_t first, std::uint64_t second) noexcept;

        // One line per frame and per draw, oldest frame first, and the copied
        // instance bytes of all draws in one block the lines point into.
        void Describe(std::vector<std::string>& lines, std::vector<std::uint8_t>& data) const;

    private:
        struct StoredDraw
        {
            RecordedDraw draw;
            std::size_t dataOffset{};
            std::size_t dataBytes{};
        };
        struct StoredNote
        {
            const char* what{};
            std::uint64_t first{};
            std::uint64_t second{};
            std::size_t beforeDraw{};
        };
        struct Frame
        {
            std::uint64_t renderIndex{};
            bool begun{};
            std::uint32_t slot{};
            std::uint32_t backBuffer{};
            std::uint32_t width{};
            std::uint32_t height{};
            std::uint64_t finishedFence{};
            std::uint64_t fence{};
            std::vector<StoredDraw> draws;
            std::vector<std::byte> data;
            std::vector<StoredNote> notes;
        };

        bool enabled_{};
        std::uint32_t current_{};
        std::array<Frame, RecordedFrames> frames_{};
    };

    // Every draw site calls this next to its draw call.
    inline void RecordDraw(const RecordedDraw& draw) noexcept
    {
        FrameDrawRecorder& recorder = FrameDrawRecorder::Instance();
        if (recorder.Enabled())
            recorder.Add(draw);
    }

    // What each number means is part of the text, for example "text instance page created (bytes, address)".
    inline void RecordFrameNote(const char* what, const std::uint64_t first = 0, const std::uint64_t second = 0) noexcept
    {
        FrameDrawRecorder& recorder = FrameDrawRecorder::Instance();
        if (recorder.Enabled())
            recorder.Note(what, first, second);
    }
}

} // namespace PhantomCore
