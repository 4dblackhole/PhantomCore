#include "Renderer/FrameImageReadbackQueue.h"

#include <cstring>
#include <exception>
#include <stdexcept>

FrameImageReadbackQueue::FrameImageReadbackQueue(ID3D12Device& device, std::uint32_t frameCount)
    : device_(device), frames_(frameCount)
{
}

void FrameImageReadbackQueue::BeginFrame(
    std::uint32_t frameIndex, std::uint64_t renderIndex, ID3D12Resource& target)
{
    frameIndex_ = frameIndex;
    renderIndex_ = renderIndex;
    target_ = &target;
    recording_ = true;
}

std::shared_future<RgbaImage> FrameImageReadbackQueue::Request(std::uint64_t renderIndex)
{
    if (!recording_ || renderIndex != renderIndex_)
        throw std::logic_error("ReadImage requires the current frame's RenderContext.");
    auto& pending = frames_.at(frameIndex_);
    if (!pending)
        pending = CreatePendingImage();
    return pending->future;
}

std::unique_ptr<FrameImageReadbackQueue::PendingImage> FrameImageReadbackQueue::CreatePendingImage() const
{
    const auto targetDescription = target_->GetDesc();
    if (targetDescription.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        throw std::logic_error("Frame image readback requires an RGBA output target.");

    auto pending = std::make_unique<PendingImage>();
    pending->image.width = static_cast<std::uint32_t>(targetDescription.Width);
    pending->image.height = targetDescription.Height;
    pending->image.pixels.resize(static_cast<std::size_t>(pending->image.width) * pending->image.height * 4);
    device_.GetCopyableFootprints(&targetDescription, 0, 1, 0, &pending->footprint, nullptr, nullptr,
                                 &pending->bufferSize);

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = pending->bufferSize;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device_.CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(pending->buffer.GetAddressOf()))))
        throw std::runtime_error("Failed to allocate a frame image readback buffer.");
    pending->future = pending->promise.get_future().share();
    return pending;
}

void FrameImageReadbackQueue::RecordCopy(ID3D12GraphicsCommandList& commands)
{
    recording_ = false;
    const auto& pending = frames_.at(frameIndex_);
    if (!pending)
        return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {target_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    commands.ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
    destination.pResource = pending->buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = pending->footprint;
    source.pResource = target_;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands.CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commands.ResourceBarrier(1, &barrier);
}

void FrameImageReadbackQueue::MarkSubmitted() noexcept
{
    if (auto& pending = frames_[frameIndex_])
        pending->submitted = true;
}

void FrameImageReadbackQueue::CopyPixels(PendingImage& pending)
{
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(pending.bufferSize)};
    if (FAILED(pending.buffer->Map(0, &range, &mapped)))
        throw std::runtime_error("Failed to map a completed frame image for CPU access.");
    const auto* bytes = static_cast<const std::uint8_t*>(mapped) + pending.footprint.Offset;
    const std::size_t rowBytes = static_cast<std::size_t>(pending.image.width) * 4;
    for (std::uint32_t row = 0; row < pending.image.height; ++row)
        std::memcpy(pending.image.pixels.data() + row * rowBytes,
                    bytes + static_cast<std::size_t>(row) * pending.footprint.Footprint.RowPitch, rowBytes);
    const D3D12_RANGE noWrites{0, 0};
    pending.buffer->Unmap(0, &noWrites);
}

void FrameImageReadbackQueue::CompleteFrame(std::uint32_t frameIndex)
{
    auto pending = std::move(frames_.at(frameIndex));
    if (!pending)
        return;
    try
    {
        if (!pending->submitted)
            throw std::runtime_error("Frame image readback was cancelled before frame submission.");
        CopyPixels(*pending);
        pending->promise.set_value(std::move(pending->image));
    }
    catch (...)
    {
        pending->promise.set_exception(std::current_exception());
    }
}

void FrameImageReadbackQueue::CompleteAll()
{
    recording_ = false;
    for (std::uint32_t index = 0; index < frames_.size(); ++index)
        CompleteFrame(index);
}
