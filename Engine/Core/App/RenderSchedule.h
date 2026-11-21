#pragma once

#include <cstdint>


namespace PhantomCore
{
// How often the main loop calls Render. Update is never limited.
enum class RenderRateMode : std::uint8_t
{
    Display,       // one frame per display refresh
    DisplayDouble, // two frames per display refresh
    Unlimited,     // one frame after every Update
};

// Lets the Client change the render rate mode while the loop runs. Run owns
// the service; calls belong to the game-loop thread and take effect on the
// next loop iteration.
class IRenderRateControl
{
  public:
    virtual ~IRenderRateControl() = default;
    [[nodiscard]] virtual RenderRateMode Mode() const noexcept = 0;
    virtual void SetMode(RenderRateMode mode) noexcept = 0;
};

// Decides on which loop iterations the engine renders. Times are the loop's
// QPC seconds. A limited mode keeps a fixed-interval deadline; a frame that
// is more than one interval late restarts the deadline instead of rendering
// a burst to catch up.
class RenderSchedule final
{
  public:
    // Base rate in Hz: the display refresh rate or the Client's override.
    // Rates of 1 Hz or less are ignored.
    void SetBaseRate(const double hz, const double now) noexcept
    {
        if (hz <= 1.0 || hz == baseRateHz_)
        {
            return;
        }
        baseRateHz_ = hz;
        nextRenderTime_ = now;
    }

    void SetMode(const RenderRateMode mode, const double now) noexcept
    {
        if (mode == mode_)
        {
            return;
        }
        mode_ = mode;
        nextRenderTime_ = now;
    }

    [[nodiscard]] RenderRateMode Mode() const noexcept
    {
        return mode_;
    }

    [[nodiscard]] double BaseRateHz() const noexcept
    {
        return baseRateHz_;
    }

    // Seconds between frames; zero when every loop iteration renders.
    [[nodiscard]] double IntervalSeconds() const noexcept
    {
        switch (mode_)
        {
        case RenderRateMode::DisplayDouble:
            return 0.5 / baseRateHz_;
        case RenderRateMode::Unlimited:
            return 0.0;
        default:
            return 1.0 / baseRateHz_;
        }
    }

    [[nodiscard]] bool IsDue(const double now) const noexcept
    {
        return mode_ == RenderRateMode::Unlimited || now >= nextRenderTime_;
    }

    // Call after a frame was rendered at `now`.
    void MarkRendered(const double now) noexcept
    {
        const double interval = IntervalSeconds();
        nextRenderTime_ += interval;
        if (nextRenderTime_ < now - interval)
        {
            nextRenderTime_ = now + interval;
        }
    }

  private:
    RenderRateMode mode_{RenderRateMode::Display};
    double baseRateHz_{60.0};
    double nextRenderTime_{};
};

} // namespace PhantomCore
