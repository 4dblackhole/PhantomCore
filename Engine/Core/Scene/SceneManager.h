#pragma once

#include "Scene/GameScene.h"

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

// Scene lifetime is a route-registration policy, not a decision made by
// the Scene object itself.
namespace PhantomCore
{
enum class SceneRetention : std::uint8_t
{
    // Construct on first entry, then retain and reuse the same object.
    KeepAlive,

    // Construct on every entry, then shut down and destroy on exit.
    DestroyOnExit,
};

// Owns all registered Scene factories and the objects created from them.
// Every transition is applied after Update returns, so an active Scene is
// never ended or destroyed from inside one of its own member functions.
class SceneManager final
{
public:
    using SceneFactory =
        std::function<std::unique_ptr<GameScene>()>;

    SceneManager() = default;
    ~SceneManager();

    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    void Initialize(const EngineServices& services);

    // Registers one route and its lifetime policy. Registration does not
    // create the Scene; creation is deferred until the route is entered.
    [[nodiscard]] bool RegisterScene(
        std::string sceneId,
        SceneRetention retention,
        SceneFactory factory);

    // Constructor arguments are copied into the factory because a
    // DestroyOnExit Scene may need to be constructed more than once.
    template <typename SceneType, typename... Arguments>
        requires std::derived_from<SceneType, GameScene> &&
            (std::copy_constructible<
                std::decay_t<Arguments>> && ...)
    [[nodiscard]] bool RegisterScene(
        std::string sceneId,
        const SceneRetention retention,
        Arguments&&... arguments)
    {
        auto constructorArguments =
            std::tuple<std::decay_t<Arguments>...>(
                std::forward<Arguments>(arguments)...);

        SceneFactory factory =
            [constructorArguments =
                 std::move(constructorArguments)]()
                -> std::unique_ptr<GameScene>
            {
                return std::apply(
                    [](const auto&... values)
                        -> std::unique_ptr<GameScene>
                    {
                        return std::make_unique<SceneType>(values...);
                    },
                    constructorArguments);
            };

        return RegisterScene(
            std::move(sceneId),
            retention,
            std::move(factory));
    }

    // Start is immediate and is intended only for Client initialization.
    // It creates the initial Scene if needed, then calls BeginScene and
    // the initial OnResize.
    [[nodiscard]] bool Start(std::string_view sceneId);

    // All Scene changes use the same ID-based, deferred route operation.
    [[nodiscard]] bool ChangeScene(std::string_view sceneId);

    void Quit() noexcept;

    [[nodiscard]] bool Update(const UpdateContext& context);
    void Render(const RenderContext& context);
    void OnResize(std::uint32_t width, std::uint32_t height);
    void Shutdown() noexcept;

    // ContainsScene reports whether an ID is registered, even if its
    // lazily created object does not exist yet.
    [[nodiscard]] bool ContainsScene(std::string_view sceneId) const;
    [[nodiscard]] std::string_view CurrentSceneId() const noexcept;
    [[nodiscard]] GameScene* CurrentScene() const noexcept;
    [[nodiscard]] bool IsCurrentSceneTransient() const noexcept;

private:
    struct RegisteredScene final
    {
        SceneRetention retention{SceneRetention::KeepAlive};
        SceneFactory factory;
        std::unique_ptr<GameScene> retainedScene;
    };

    enum class PendingAction
    {
        None,
        Change,
        Quit,
    };

    [[nodiscard]] RegisteredScene* FindRegistration(
        std::string_view sceneId);
    [[nodiscard]] const RegisteredScene* FindRegistration(
        std::string_view sceneId) const;
    [[nodiscard]] std::unique_ptr<GameScene> CreateScene(
        const RegisteredScene& registration) const;
    [[nodiscard]] GameScene* PrepareScene(
        RegisteredScene& registration,
        std::unique_ptr<GameScene>& transientScene);
    [[nodiscard]] bool ApplyPendingAction();
    void ActivateScene(GameScene& scene, std::string sceneId);
    void EndCurrentScene();
    void DestroyActiveTransientScene() noexcept;

    const EngineServices* services_{};
    std::unordered_map<std::string, RegisteredScene> registrations_;
    std::unique_ptr<GameScene> transientScene_;
    GameScene* currentScene_{};
    std::string currentSceneId_;
    std::string pendingSceneId_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    PendingAction pendingAction_{PendingAction::None};
    bool initialized_{};
    bool started_{};
};

} // namespace PhantomCore
