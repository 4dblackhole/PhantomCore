#pragma once

#include "Client/IGameClient.h"

#include <cstdint>

namespace PhantomCore
{
class SceneManager;

// Base lifecycle for one switchable Client scene.
class GameScene
{
public:
    virtual ~GameScene();

    // Called once per object, when its registered route is first entered.
    // DestroyOnExit routes create a fresh object on every entry, so each
    // new object receives its own Initialize/Shutdown pair.
    virtual void Initialize(const EngineServices& services);

    // Called every time this scene becomes or ceases to be active.
    virtual void BeginScene();
    virtual void EndScene() noexcept;

    virtual void OnResize(std::uint32_t width, std::uint32_t height);
    virtual void Update(
        const UpdateContext& context,
        SceneManager& scenes) = 0;
    virtual void Render(const RenderContext& context) = 0;

    // Called once before the manager releases the scene. Renderer frame
    // resources retain submitted GPU handles until their fences complete.
    virtual void Shutdown() noexcept;
};

} // namespace PhantomCore
