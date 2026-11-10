#include "SceneManagerTests.h"
#include "PhantomCore.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace
{
    enum class Phase { Initialize, Begin, Resize, UpdateReturned, End, Shutdown, Destroy };

    struct Event final
    {
        char scene;
        Phase phase;
        bool operator==(const Event&) const = default;
    };

    void Require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    struct Trace final
    {
        void Record(char scene, Phase phase) noexcept
        {
            if (count < events.size())
                events[count] = {scene, phase};
            ++count;
        }

        void Expect(std::initializer_list<Event> expected, const char* message)
        {
            Require(count == expected.size() && count <= events.size() &&
                        std::equal(expected.begin(), expected.end(), events.begin()), message);
            count = 0;
        }

        std::array<Event, 16> events{};
        std::size_t count{};
    };

    class ProbeScene final : public GameScene
    {
    public:
        ProbeScene(Trace& trace, char id, std::string_view next) : trace_(trace), id_(id), next_(next) {}
        ~ProbeScene() override { trace_.Record(id_, Phase::Destroy); }
        void Initialize(const EngineServices&) override { trace_.Record(id_, Phase::Initialize); }
        void BeginScene() override { trace_.Record(id_, Phase::Begin); }
        void OnResize(std::uint32_t, std::uint32_t) override { trace_.Record(id_, Phase::Resize); }
        void EndScene() noexcept override { trace_.Record(id_, Phase::End); }
        void Shutdown() noexcept override { trace_.Record(id_, Phase::Shutdown); }
        void Render(const RenderContext&) override {}

        void Update(const UpdateContext&, SceneManager& scenes) override
        {
            Require(scenes.ChangeScene(next_), "The active Scene must be able to request a transition.");
            Require(scenes.CurrentScene() == this, "A transition must leave the caller active until Update returns.");
            trace_.Record(id_, Phase::UpdateReturned);
        }

    private:
        Trace& trace_;
        char id_;
        std::string_view next_;
    };

    void CheckSceneLifecycle(const EngineServices& services)
    {
        using enum Phase;
        Trace trace;
        SceneManager scenes;
        scenes.Initialize(services);
        Require(scenes.RegisterScene("A", SceneRetention::KeepAlive,
                    [&] { return std::make_unique<ProbeScene>(trace, 'A', "B"); }), "Register retained Scene.");
        Require(scenes.RegisterScene("B", SceneRetention::DestroyOnExit,
                    [&] { return std::make_unique<ProbeScene>(trace, 'B', "A"); }), "Register transient Scene.");
        trace.Expect({}, "Registration must defer construction.");
        Require(scenes.Start("A"), "Start retained Scene.");
        trace.Expect({{'A', Initialize}, {'A', Begin}, {'A', Resize}}, "Initial activation order.");

        const InputState input;
        const UpdateContext context{0, 0, 0, input, services.audio, {}};
        for (int visit = 0; visit < 2; ++visit)
        {
            Require(scenes.Update(context) && scenes.CurrentSceneId() == "B", "Enter transient Scene.");
            trace.Expect({{'A', UpdateReturned}, {'B', Initialize}, {'A', End}, {'B', Begin}, {'B', Resize}},
                         "Prepare each new transient Scene after Update and before ending the retained Scene.");
            Require(scenes.Update(context) && scenes.CurrentSceneId() == "A", "Return to retained Scene.");
            trace.Expect({{'B', UpdateReturned}, {'B', End}, {'B', Shutdown}, {'B', Destroy}, {'A', Begin}, {'A', Resize}},
                         "Destroy the transient Scene on exit and reactivate the retained Scene without reinitializing it.");
        }

        scenes.Shutdown();
        trace.Expect({{'A', End}, {'A', Shutdown}, {'A', Destroy}}, "Shutdown releases the retained Scene once.");
    }

    class SceneTestClient final : public IGameClient
    {
    public:
        EngineConfig GetEngineConfig() const override
        {
            EngineConfig config;
            config.showWindow = false;
            config.audio.preferredBackend = AudioOutputBackend::NoSound;
            return config;
        }
        void Initialize(const EngineServices& services) override
        {
            try
            {
                CheckSceneLifecycle(services);
            }
            catch (const std::exception& error)
            {
                std::cerr << "Scene lifecycle failed: " << error.what() << '\n';
                throw;
            }
        }
        bool Update(const UpdateContext&) override { return false; }
        void Render(const RenderContext&) override {}
        void OnResize(std::uint32_t, std::uint32_t) override {}
        void Shutdown() noexcept override {}
    };
}

int TestSceneManager()
{
    const int result = Run(std::make_unique<SceneTestClient>());
    if (result == 0)
        std::cout << "Scene lifecycle: deferred transitions, retention and shutdown passed.\n";
    return result;
}
