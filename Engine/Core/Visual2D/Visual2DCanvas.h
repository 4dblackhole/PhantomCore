#pragma once

#include "Visual2D/Visual2DNode.h"

#include <array>
#include <optional>
#include <vector>


namespace PhantomCore
{
enum class CanvasScaleMode : std::uint8_t
{
    Fixed,
    FixedHeight,
};

// Owns one Visual2D tree and nine non-rendering anchor nodes. Canvas-local
// coordinates are centered with positive Y pointing upward. A Canvas may
// cover the whole viewport or only a panel-sized logical region. FixedHeight
// keeps the logical height constant and expands only the logical width.
class Visual2DCanvas final
{
public:
    explicit Visual2DCanvas(
        Size referenceSize = {1280.0F, 720.0F},
        CanvasScaleMode scaleMode = CanvasScaleMode::FixedHeight);

    [[nodiscard]] Size ReferenceSize() const noexcept;
    [[nodiscard]] Size LogicalSize() const noexcept;
    [[nodiscard]] Size ViewportSize() const noexcept;
    [[nodiscard]] float PixelScale() const noexcept;
    [[nodiscard]] CanvasScaleMode ScaleMode() const noexcept;
    void SetViewportSize(Size viewportSize);
    void SetFixedLogicalSize(Size logicalSize);

    [[nodiscard]] Visual2DNode& Root() noexcept;
    [[nodiscard]] const Visual2DNode& Root() const noexcept;
    [[nodiscard]] Visual2DNode& AnchorNode(Anchor anchor) noexcept;
    [[nodiscard]] const Visual2DNode& AnchorNode(Anchor anchor) const noexcept;
    [[nodiscard]] Visual2DNode& CreateNode(
        Anchor anchor = Anchor::TopLeft,
        std::string name = {});
    [[nodiscard]] Visual2DNode* FindNode(NodeId id) noexcept;
    [[nodiscard]] const Visual2DNode* FindNode(NodeId id) const noexcept;
    [[nodiscard]] bool RemoveNode(NodeId id) noexcept;

    void Update(double elapsedSeconds);
    [[nodiscard]] std::vector<DrawPacket> BuildDrawList() const;
    [[nodiscard]] std::vector<Action> TakeActions();

private:
    friend class Visual2DInputRouter;

    [[nodiscard]] Visual2DNode::HitResult HitTest(Point position);
    void CreateAnchors();
    void UpdateAnchorTransforms();
    [[nodiscard]] static std::size_t AnchorIndex(Anchor anchor) noexcept;
    [[nodiscard]] static Point AnchorPivot(Anchor anchor) noexcept;

    Size referenceSize_{};
    Size logicalSize_{};
    Size viewportSize_{};
    float pixelScale_{1.0F};
    CanvasScaleMode scaleMode_{CanvasScaleMode::FixedHeight};
    Visual2DNode root_{"Visual2DCanvas.Root"};
    std::array<NodeId, 9> anchorIds_{};
    std::vector<Action> actions_;
};

// Converts an absolute top-left-origin screen pixel to centered, Y-up
// Canvas-local coordinates. The viewport clips only the physical screen;
// points outside the Canvas are intentionally preserved so pointer capture
// can continue while dragging.
[[nodiscard]] std::optional<Point> MapScreenPointer(
    Point screenPosition,
    Size viewportSize,
    const Visual2DCanvas& canvas,
    Point canvasOrigin = {}) noexcept;

} // namespace PhantomCore
