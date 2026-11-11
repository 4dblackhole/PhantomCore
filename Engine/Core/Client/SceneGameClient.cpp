#include "Client/SceneGameClient.h"

#include <stdexcept>


namespace PhantomCore
{
SceneGameClient::~SceneGameClient() = default;

void SceneGameClient::Initialize(const EngineServices& services)
{
    if (initialized_)
    {
        throw std::logic_error("SceneGameClient is already initialized.");
    }

    screenVisuals_.Initialize(
        services.visual2DRendering,
        {static_cast<float>(services.windowWidth),
            static_cast<float>(services.windowHeight)});
    scenes_.Initialize(services);
    try
    {
        RegisterScenes(scenes_);
        if (!scenes_.Start(InitialSceneId()))
        {
            throw std::runtime_error(
                "The initial game scene is not registered.");
        }
        OnClientInitialized(services);
        initialized_ = true;
    }
    catch (...)
    {
        // A derived Client may have created root-level resources in its
        // initialization hook before a later step throws.
        OnClientShuttingDown();
        scenes_.Shutdown();
        screenVisuals_.Shutdown();
        audioPlayback_.StopAll();
        throw;
    }
}

bool SceneGameClient::Update(const UpdateContext& context)
{
    if (!initialized_)
    {
        return false;
    }

    screenVisuals_.Update(context.deltaSeconds);
    const bool keepRunning = scenes_.Update(context);
    OnClientUpdated(context);
    playbackMaintenanceSeconds_ += context.deltaSeconds;
    if (playbackMaintenanceSeconds_ >= 0.002)
    {
        playbackMaintenanceSeconds_ = 0;
        audioPlayback_.Update();
    }
    return keepRunning;
}

void SceneGameClient::Render(
    const RenderContext& context)
{
    if (initialized_)
    {
        scenes_.Render(context);
        screenVisuals_.Render(context);
        OnClientRendered(context);
    }
}

void SceneGameClient::OnResize(
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (initialized_)
    {
        screenVisuals_.OnResize(width, height);
        scenes_.OnResize(width, height);
    }
}

void SceneGameClient::Shutdown() noexcept
{
    OnClientShuttingDown();
    scenes_.Shutdown();
    screenVisuals_.Shutdown();
    audioPlayback_.StopAll();
    initialized_ = false;
    playbackMaintenanceSeconds_ = 0;
}

AudioPlaybackManager& SceneGameClient::AudioPlayback() noexcept
{
    return audioPlayback_;
}

const AudioPlaybackManager& SceneGameClient::AudioPlayback() const noexcept
{
    return audioPlayback_;
}

ScreenVisual2DManager& SceneGameClient::ScreenVisuals() noexcept
{
    return screenVisuals_;
}

const ScreenVisual2DManager&
    SceneGameClient::ScreenVisuals() const noexcept
{
    return screenVisuals_;
}

SceneManager& SceneGameClient::Scenes() noexcept
{
    return scenes_;
}

const SceneManager& SceneGameClient::Scenes() const noexcept
{
    return scenes_;
}

void SceneGameClient::OnClientInitialized(const EngineServices&)
{
}

void SceneGameClient::OnClientUpdated(const UpdateContext&)
{
}

void SceneGameClient::OnClientRendered(const RenderContext&)
{
}

void SceneGameClient::OnClientShuttingDown() noexcept
{
}

} // namespace PhantomCore
