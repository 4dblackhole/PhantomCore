#pragma once

#include <algorithm>
#include <cstdint>


namespace PhantomCore
{
// Most frames per second while the window is shown but another program's
// window is in front. EngineConfig::backgroundRenderRateHz starts from it.
inline constexpr double DefaultBackgroundRenderRateHz = 30.0;

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

    // Cap while in the background. Rates of 1 Hz or less turn the cap off.
    void SetBackgroundRate(const double hz) noexcept
    {
        backgroundRateHz_ = hz > 1.0 ? hz : 0.0;
    }

    // A change takes effect at once: the next frame is due at `now`, so a
    // window brought to the front is drawn on the same loop iteration.
    void SetBackground(const bool background, const double now) noexcept
    {
        if (background == background_)
        {
            return;
        }
        background_ = background;
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

    // True while the background cap slows rendering down.
    [[nodiscard]] bool IsBackgroundLimited() const noexcept
    {
        return background_ && backgroundRateHz_ > 0.0;
    }

    // Seconds between frames; zero when every loop iteration renders.
    [[nodiscard]] double IntervalSeconds() const noexcept
    {
        const double interval = ModeIntervalSeconds();
        if (!IsBackgroundLimited())
        {
            return interval;
        }
        return std::max(interval, 1.0 / backgroundRateHz_);
    }

    [[nodiscard]] bool IsDue(const double now) const noexcept
    {
        return IntervalSeconds() == 0.0 || now >= nextRenderTime_;
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
    [[nodiscard]] double ModeIntervalSeconds() const noexcept
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

    RenderRateMode mode_{RenderRateMode::Display};
    double baseRateHz_{60.0};
    double backgroundRateHz_{};
    bool background_{};
    double nextRenderTime_{};
};

} // namespace PhantomCore
