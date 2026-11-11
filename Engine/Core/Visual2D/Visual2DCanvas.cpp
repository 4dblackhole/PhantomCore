#include "Visual2D/Visual2DCanvas.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>


namespace PhantomCore
{
namespace
{
    [[nodiscard]] bool IsPositiveFinite(const Size size) noexcept
    {
        return std::isfinite(size.width) && std::isfinite(size.height) &&
            size.width > 0.0F && size.height > 0.0F;
    }
}

Visual2DCanvas::Visual2DCanvas(
    const Size referenceSize,
    const CanvasScaleMode scaleMode)
    : referenceSize_(referenceSize),
      logicalSize_(referenceSize),
      viewportSize_(referenceSize),
      scaleMode_(scaleMode)
{
    if (!IsPositiveFinite(referenceSize_))
    {
        throw std::invalid_argument(
            "A Visual2D Canvas requires a positive reference size.");
    }
    root_.SetSize(referenceSize_);
    CreateAnchors();
    UpdateAnchorTransforms();
}

Size Visual2DCanvas::ReferenceSize() const noexcept
{
    return referenceSize_;
}

Size Visual2DCanvas::LogicalSize() const noexcept
{
    return logicalSize_;
}

Size Visual2DCanvas::ViewportSize() const noexcept
{
    return viewportSize_;
}

float Visual2DCanvas::PixelScale() const noexcept
{
    return pixelScale_;
}

CanvasScaleMode Visual2DCanvas::ScaleMode() const noexcept
{
    return scaleMode_;
}

void Visual2DCanvas::SetViewportSize(const Size viewportSize)
{
    if (!IsPositiveFinite(viewportSize))
    {
        throw std::invalid_argument(
            "A Visual2D viewport requires a positive size.");
    }
    viewportSize_ = viewportSize;
    if (scaleMode_ == CanvasScaleMode::FixedHeight)
    {
        pixelScale_ = viewportSize.height / referenceSize_.height;
        logicalSize_ = {
            viewportSize.width / pixelScale_,
            referenceSize_.height};
    }
    else
    {
        pixelScale_ = 1.0F;
        logicalSize_ = referenceSize_;
    }
    root_.SetSize(logicalSize_);
    UpdateAnchorTransforms();
}

void Visual2DCanvas::SetFixedLogicalSize(const Size logicalSize)
{
    if (!IsPositiveFinite(logicalSize))
    {
        throw std::invalid_argument(
            "A fixed Visual2D Canvas requires a positive size.");
    }
    scaleMode_ = CanvasScaleMode::Fixed;
    referenceSize_ = logicalSize;
    logicalSize_ = logicalSize;
    viewportSize_ = logicalSize;
    pixelScale_ = 1.0F;
    root_.SetSize(logicalSize_);
    UpdateAnchorTransforms();
}

Visual2DNode& Visual2DCanvas::Root() noexcept
{
    return root_;
}

const Visual2DNode& Visual2DCanvas::Root() const noexcept
{
    return root_;
}

Visual2DNode& Visual2DCanvas::AnchorNode(const Anchor anchor) noexcept
{
    return *root_.Find(anchorIds_[AnchorIndex(anchor)]);
}

const Visual2DNode& Visual2DCanvas::AnchorNode(
    const Anchor anchor) const noexcept
{
    return *root_.Find(anchorIds_[AnchorIndex(anchor)]);
}

Visual2DNode& Visual2DCanvas::CreateNode(
    const Anchor anchor,
    std::string name)
{
    auto node = std::make_unique<Visual2DNode>(std::move(name));
    node->SetPivot(AnchorPivot(anchor));
    return AnchorNode(anchor).AddChild(std::move(node));
}

Visual2DNode* Visual2DCanvas::FindNode(const NodeId id) noexcept
{
    if (id == 0)
    {
        return nullptr;
    }
    return root_.Find(id);
}

const Visual2DNode* Visual2DCanvas::FindNode(const NodeId id) const noexcept
{
    if (id == 0)
    {
        return nullptr;
    }
    return root_.Find(id);
}

bool Visual2DCanvas::RemoveNode(const NodeId id) noexcept
{
    if (id == root_.Id() ||
        std::find(anchorIds_.begin(), anchorIds_.end(), id) !=
            anchorIds_.end())
    {
        return false;
    }
    if (Visual2DNode* node = root_.Find(id);
        node != nullptr && node->Parent() != nullptr)
    {
        return node->Parent()->RemoveChild(id);
    }
    return false;
}

void Visual2DCanvas::Update(const double elapsedSeconds)
{
    root_.UpdateRecursive(elapsedSeconds);
}

std::vector<DrawPacket> Visual2DCanvas::BuildDrawList() const
{
    std::vector<DrawPacket> packets;
    root_.CollectDrawPackets(packets);
    return packets;
}

std::vector<Action> Visual2DCanvas::TakeActions()
{
    std::vector<Action> result = std::move(actions_);
    actions_.clear();
    return result;
}

Visual2DNode::HitResult Visual2DCanvas::HitTest(
    const Point position)
{
    return root_.HitTest(position);
}

void Visual2DCanvas::CreateAnchors()
{
    static constexpr const char* names[]{
        "Anchor.TopLeft",
        "Anchor.TopCenter",
        "Anchor.TopRight",
        "Anchor.MiddleLeft",
        "Anchor.Center",
        "Anchor.MiddleRight",
        "Anchor.BottomLeft",
        "Anchor.BottomCenter",
        "Anchor.BottomRight"};
    for (std::size_t index = 0; index < anchorIds_.size(); ++index)
    {
        Visual2DNode& anchor = root_.CreateChild(names[index]);
        anchorIds_[index] = anchor.Id();
    }
}

void Visual2DCanvas::UpdateAnchorTransforms()
{
    const float halfWidth = logicalSize_.width * 0.5F;
    const float halfHeight = logicalSize_.height * 0.5F;
    const float xPositions[]{-halfWidth, 0.0F, halfWidth};
    const float yPositions[]{halfHeight, 0.0F, -halfHeight};
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t column = 0; column < 3; ++column)
        {
            AnchorNode(static_cast<Anchor>(row * 3 + column)).
                Transform().SetPosition(
                    xPositions[column], yPositions[row], 0.0F);
        }
    }
}

std::size_t Visual2DCanvas::AnchorIndex(const Anchor anchor) noexcept
{
    return static_cast<std::size_t>(anchor);
}

Point Visual2DCanvas::AnchorPivot(const Anchor anchor) noexcept
{
    const std::size_t index = AnchorIndex(anchor);
    return {
        static_cast<float>(index % 3) * 0.5F,
        1.0F - static_cast<float>(index / 3) * 0.5F};
}

std::optional<Point> MapScreenPointer(
    const Point screenPosition,
    const Size viewportSize,
    const Visual2DCanvas& canvas,
    const Point canvasOrigin) noexcept
{
    if (!IsPositiveFinite(viewportSize) || canvas.PixelScale() <= 0.0F)
    {
        return std::nullopt;
    }
    if (screenPosition.x < 0.0F || screenPosition.y < 0.0F ||
        screenPosition.x > viewportSize.width ||
        screenPosition.y > viewportSize.height)
    {
        return std::nullopt;
    }
    const Point localPixels{
        screenPosition.x - canvasOrigin.x,
        screenPosition.y - canvasOrigin.y};
    return Point{
        localPixels.x / canvas.PixelScale() -
            canvas.LogicalSize().width * 0.5F,
        canvas.LogicalSize().height * 0.5F -
            localPixels.y / canvas.PixelScale()};
}

} // namespace PhantomCore
