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

    class FrameDrawRecorder final
    {
    public:
        // The one recorder of this thread's renderer.
        [[nodiscard]] static FrameDrawRecorder& Instance() noexcept;

        void Enable(bool enabled) noexcept;
        [[nodiscard]] bool Enabled() const noexcept { return enabled_; }

        // The frame ring slot is the key: its buffers are the ones the slot's draws read.
        void BeginFrame(std::uint32_t frameIndex, std::uint64_t renderIndex) noexcept;
        void Add(const RecordedDraw& draw) noexcept;

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
        struct Frame
        {
            std::uint64_t renderIndex{};
            bool begun{};
            std::vector<StoredDraw> draws;
            std::vector<std::byte> data;
        };

        bool enabled_{};
        std::uint32_t current_{};
        std::array<Frame, FrameCount> frames_{};
    };

    // Every draw site calls this next to its draw call.
    inline void RecordDraw(const RecordedDraw& draw) noexcept
    {
        FrameDrawRecorder& recorder = FrameDrawRecorder::Instance();
        if (recorder.Enabled())
            recorder.Add(draw);
    }
}

} // namespace PhantomCore
