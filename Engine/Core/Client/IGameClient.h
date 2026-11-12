#pragma once

#include "Input/Input.h"
#include "Mesh/MeshRendering.h"
#include "Rendering/RenderContext.h"
#include "System/AudioSystem.h"
#include "Text/TextRendering.h"
#include "Window/WindowControl.h"

#include <cstdint>
#include <string>


// Engine-owned measurement data. The Client decides whether, where, and
// how to present these values; an unavailable sample has `hasMeasurement`
// set to false during the first reporting interval.
namespace PhantomCore
{
struct PerformanceStatistics final
{
    std::uint64_t framesPerSecond{};
    std::uint64_t updatesPerSecond{};
    // GPU draw calls recorded per rendered frame in the interval: rounded
    // average and largest single frame. Batching merges consecutive
    // primitives that share a pipeline and texture page into one draw.
    std::uint64_t drawCallsPerFrame{};
    std::uint64_t peakDrawCallsPerFrame{};
    std::uint64_t measurementIndex{};
    bool hasMeasurement{};
};

// Returned before any runtime subsystem exists. It lets the Client
// describe the window, scheduling, and audio requirements to Run.
struct EngineConfig
{
    std::wstring windowTitle{L"My Rhythm Game"};
    std::uint32_t windowWidth{1280};
    std::uint32_t windowHeight{720};
    ClearColor clearColor{
        240.0F / 255.0F,
        248.0F / 255.0F,
        1.0F,
        1.0F};

    AudioConfig audio{};
    AudioBackendFactory audioBackendFactory{};
    AudioClipBackendFactory audioClipBackendFactory{};
    double audioUpdateRateHz{500.0};
    double renderRateOverrideHz{};
    double maximumUpdateDeltaSeconds{0.1};

    // Hidden runs also suppress modal fatal-error dialogs; errors remain logged.
    bool showWindow{true};
    std::uint64_t autoExitAfterRenderedFrames{};
};

// Non-owning services created by Run. They remain valid from Initialize
// through the start of Shutdown; Client code must not release them or
// retain them after Shutdown.
struct EngineServices
{
    MeshRenderSystem& meshRendering;
    TextRenderSystem& textRendering;
    Visual2DRenderSystem& visual2DRendering;
    AudioSystem& audio;
    std::uint32_t windowWidth{};
    std::uint32_t windowHeight{};
    IWindowControl *windowControl{}; // Nullable in offscreen/test contexts.
};

// One Update invocation. deltaSeconds is QPC-derived and clamped by
// EngineConfig. Input carries both current state and ordered Raw Input
// events with their original QPC timestamps.
struct UpdateContext
{
    double deltaSeconds{};
    double totalSeconds{};
    std::uint64_t updateIndex{};
    const InputState& input;
    AudioSystem& audio;
    PerformanceStatistics performance;
};

// Contract implemented by a game-specific Client project.
// Normal lifecycle: GetEngineConfig -> Initialize ->
// [Update / Render / OnResize]* -> Shutdown.
class IGameClient
{
public:
    virtual ~IGameClient();

    [[nodiscard]] virtual EngineConfig GetEngineConfig() const = 0;
    virtual void Initialize(const EngineServices& services) = 0;
    [[nodiscard]] virtual bool Update(const UpdateContext& context) = 0;
    virtual void Render(const RenderContext& context) = 0;
    virtual void OnResize(std::uint32_t width, std::uint32_t height) = 0;
    virtual void Shutdown() noexcept = 0;
};

} // namespace PhantomCore
