#pragma once

#include "Rendering/ImageReadback.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <future>
#include <memory>
#include <vector>

// Internal readback storage follows the renderer's existing frame-resource fences.
// The renderer completes a slot only after its normal frame wait or full GPU wait.
namespace PhantomCore
{
class FrameImageReadbackQueue final
{
public:
    FrameImageReadbackQueue(ID3D12Device& device, std::uint32_t frameCount);

    void BeginFrame(std::uint32_t frameIndex, std::uint64_t renderIndex, ID3D12Resource& target);
    [[nodiscard]] std::shared_future<RgbaImage> Request(std::uint64_t renderIndex);
    void RecordCopy(ID3D12GraphicsCommandList& commands);
    void MarkSubmitted() noexcept;
    void CompleteFrame(std::uint32_t frameIndex);
    void CompleteAll();

private:
    struct PendingImage final
    {
        RgbaImage image;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::uint64_t bufferSize{};
        std::promise<RgbaImage> promise;
        std::shared_future<RgbaImage> future;
        bool submitted{};
    };

    [[nodiscard]] std::unique_ptr<PendingImage> CreatePendingImage() const;
    static void CopyPixels(PendingImage& pending);

    ID3D12Device& device_;
    std::vector<std::unique_ptr<PendingImage>> frames_;
    ID3D12Resource* target_{};
    std::uint32_t frameIndex_{};
    std::uint64_t renderIndex_{};
    bool recording_{};
};

} // namespace PhantomCore
