#include "Scene/SceneManager.h"

#include <stdexcept>

namespace PhantomCore
{
SceneManager::~SceneManager()
{
    Shutdown();
}

void SceneManager::Initialize(const EngineServices& services)
{
    if (initialized_)
    {
        throw std::logic_error("SceneManager is already initialized.");
    }

    services_ = &services;
    width_ = services.windowWidth;
    height_ = services.windowHeight;
    initialized_ = true;
}

bool SceneManager::RegisterScene(
    std::string sceneId,
    const SceneRetention retention,
    SceneFactory factory)
{
    if (!initialized_)
    {
        throw std::logic_error(
            "SceneManager must be initialized before registering scenes.");
    }
    if (started_ || sceneId.empty() || !factory ||
        registrations_.contains(sceneId))
    {
        return false;
    }

    return registrations_.emplace(
        std::move(sceneId),
        RegisteredScene{retention, std::move(factory), nullptr})
        .second;
}

bool SceneManager::Start(const std::string_view sceneId)
{
    if (!initialized_)
    {
        throw std::logic_error(
            "SceneManager must be initialized before it is started.");
    }
    if (started_)
    {
        return false;
    }

    RegisteredScene* registration = FindRegistration(sceneId);
    if (registration == nullptr)
    {
        return false;
    }

    std::unique_ptr<GameScene> nextTransientScene;
    GameScene* scene = PrepareScene(
        *registration,
        nextTransientScene);
    if (scene == nullptr)
    {
        return false;
    }

    if (registration->retention == SceneRetention::DestroyOnExit)
    {
        transientScene_ = std::move(nextTransientScene);
        scene = transientScene_.get();
    }

    started_ = true;
    // Start is the only immediate transition; it happens before the
    // Client enters its first Update.
    ActivateScene(*scene, std::string(sceneId));
    return true;
}

bool SceneManager::ChangeScene(const std::string_view sceneId)
{
    if (!started_ || pendingAction_ != PendingAction::None ||
        FindRegistration(sceneId) == nullptr)
    {
        return false;
    }

    if (sceneId == currentSceneId_)
    {
        return true;
    }

    pendingSceneId_ = sceneId;
    pendingAction_ = PendingAction::Change;
    return true;
}

void SceneManager::Quit() noexcept
{
    if (started_)
    {
        pendingSceneId_.clear();
        pendingAction_ = PendingAction::Quit;
    }
}

bool SceneManager::Update(const UpdateContext& context)
{
    if (!started_ || currentScene_ == nullptr)
    {
        return false;
    }

    currentScene_->Update(context, *this);
    // A Scene may request ChangeScene or Quit above. Apply it only after
    // Update returns, so currentScene_ cannot be destroyed mid-call.
    return ApplyPendingAction();
}

void SceneManager::Render(const RenderContext& context)
{
    if (started_ && currentScene_ != nullptr)
    {
        currentScene_->Render(context);
    }
}

void SceneManager::OnResize(
    const std::uint32_t width,
    const std::uint32_t height)
{
    width_ = width;
    height_ = height;
    if (started_ && currentScene_ != nullptr)
    {
        currentScene_->OnResize(width_, height_);
    }
}

void SceneManager::Shutdown() noexcept
{
    if (!initialized_)
    {
        return;
    }

    if (started_ && currentScene_ != nullptr)
    {
        EndCurrentScene();
    }

    currentScene_ = nullptr;
    currentSceneId_.clear();
    pendingSceneId_.clear();
    pendingAction_ = PendingAction::None;
    started_ = false;

    // Normal engine shutdown waits for the GPU before reaching this
    // function. Submitted mesh/material handles are also retained by
    // their renderer frame resources during ordinary Scene transitions.
    DestroyActiveTransientScene();

    for (auto& [sceneId, registration] : registrations_)
    {
        static_cast<void>(sceneId);
        if (registration.retainedScene != nullptr)
        {
            registration.retainedScene->Shutdown();
            registration.retainedScene.reset();
        }
    }
    registrations_.clear();

    services_ = nullptr;
    width_ = 0;
    height_ = 0;
    initialized_ = false;
}

bool SceneManager::ContainsScene(const std::string_view sceneId) const
{
    return FindRegistration(sceneId) != nullptr;
}

std::string_view SceneManager::CurrentSceneId() const noexcept
{
    return currentSceneId_;
}

GameScene* SceneManager::CurrentScene() const noexcept
{
    return currentScene_;
}

bool SceneManager::IsCurrentSceneTransient() const noexcept
{
    return transientScene_ != nullptr &&
        currentScene_ == transientScene_.get();
}

SceneManager::RegisteredScene* SceneManager::FindRegistration(
    const std::string_view sceneId)
{
    const auto iterator = registrations_.find(std::string(sceneId));
    return iterator != registrations_.end() ? &iterator->second : nullptr;
}

const SceneManager::RegisteredScene* SceneManager::FindRegistration(
    const std::string_view sceneId) const
{
    const auto iterator = registrations_.find(std::string(sceneId));
    return iterator != registrations_.end() ? &iterator->second : nullptr;
}

std::unique_ptr<GameScene> SceneManager::CreateScene(
    const RegisteredScene& registration) const
{
    if (services_ == nullptr || !registration.factory)
    {
        throw std::logic_error(
            "A registered Scene cannot be created without services and a factory.");
    }

    std::unique_ptr<GameScene> scene = registration.factory();
    if (scene == nullptr)
    {
        throw std::runtime_error("A registered Scene factory returned null.");
    }

    try
    {
        scene->Initialize(*services_);
    }
    catch (...)
    {
        scene->Shutdown();
        throw;
    }
    return scene;
}

GameScene* SceneManager::PrepareScene(
    RegisteredScene& registration,
    std::unique_ptr<GameScene>& transientScene)
{
    if (registration.retention == SceneRetention::KeepAlive)
    {
        if (registration.retainedScene == nullptr)
        {
            registration.retainedScene = CreateScene(registration);
        }
        return registration.retainedScene.get();
    }

    transientScene = CreateScene(registration);
    return transientScene.get();
}

bool SceneManager::ApplyPendingAction()
{
    switch (pendingAction_)
    {
    case PendingAction::None:
        return true;

    case PendingAction::Quit:
        EndCurrentScene();
        currentScene_ = nullptr;
        currentSceneId_.clear();
        pendingAction_ = PendingAction::None;
        started_ = false;
        return false;

    case PendingAction::Change:
    {
        RegisteredScene* registration =
            FindRegistration(pendingSceneId_);
        if (registration == nullptr)
        {
            pendingSceneId_.clear();
            pendingAction_ = PendingAction::None;
            return true;
        }

        // Prepare the target before ending the active Scene. If creation
        // fails, the old Scene remains active and owns all of its state.
        std::unique_ptr<GameScene> nextTransientScene;
        GameScene* nextScene = PrepareScene(
            *registration,
            nextTransientScene);
        std::string nextSceneId = std::move(pendingSceneId_);
        pendingSceneId_.clear();
        pendingAction_ = PendingAction::None;

        EndCurrentScene();
        DestroyActiveTransientScene();

        if (registration->retention ==
            SceneRetention::DestroyOnExit)
        {
            transientScene_ = std::move(nextTransientScene);
            nextScene = transientScene_.get();
        }

        ActivateScene(*nextScene, std::move(nextSceneId));
        return true;
    }
    }

    return true;
}

void SceneManager::ActivateScene(
    GameScene& scene,
    std::string sceneId)
{
    currentScene_ = &scene;
    currentSceneId_ = std::move(sceneId);
    currentScene_->BeginScene();
    currentScene_->OnResize(width_, height_);
}

void SceneManager::EndCurrentScene()
{
    if (currentScene_ != nullptr)
    {
        currentScene_->EndScene();
    }
}

void SceneManager::DestroyActiveTransientScene() noexcept
{
    if (transientScene_ == nullptr)
    {
        return;
    }

    if (currentScene_ == transientScene_.get())
    {
        currentScene_ = nullptr;
    }
    transientScene_->Shutdown();
    transientScene_.reset();
}

} // namespace PhantomCore
