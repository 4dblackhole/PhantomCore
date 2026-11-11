#include "Scene/GameScene.h"

namespace PhantomCore
{
GameScene::~GameScene() = default;

void GameScene::Initialize(const EngineServices&)
{
}

void GameScene::BeginScene()
{
}

void GameScene::EndScene() noexcept
{
}

void GameScene::OnResize(std::uint32_t, std::uint32_t)
{
}

void GameScene::Shutdown() noexcept
{
}

} // namespace PhantomCore
