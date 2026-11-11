#include "App/Engine.h"
#include "Renderer/D3D12Renderer.h"

#include "System/ComApartment.h"
#include "System/HighResolutionClock.h"
#include "Window/Win32Window.h"

#include <Windows.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


namespace
{
    GraphicsDiagnostics ReadGraphicsDiagnostics(ID3D12InfoQueue& queue) noexcept
    {
        using Status = GraphicsDiagnostics::Status;
        using Severity = GraphicsDiagnostics::Severity;
        try
        {
            GraphicsDiagnostics result{Status::Collected, {}};
            std::vector<std::byte> storage;
            const auto count = queue.GetNumStoredMessages();
            for (UINT64 index = 0; index < count; ++index)
            {
                SIZE_T bytes{};
                if (FAILED(queue.GetMessage(index, nullptr, &bytes)))
                    return {Status::ReadFailed, {}};
                storage.resize(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (FAILED(queue.GetMessage(index, message, &bytes)))
                    return {Status::ReadFailed, {}};
                switch (message->Severity)
                {
                case D3D12_MESSAGE_SEVERITY_ERROR:
                    result.messages.push_back({Severity::Error, message->pDescription});
                    break;
                case D3D12_MESSAGE_SEVERITY_CORRUPTION:
                    result.messages.push_back({Severity::Corruption, message->pDescription});
                    break;
                default:
                    break;
                }
            }
            return result;
        }
        catch (...)
        {
            return {Status::ReadFailed, {}};
        }
    }

    double ValidRate(const double rate, const double fallback) noexcept
    {
        return rate > 1.0 ? rate : fallback;
    }

    void ReportFatalError(const std::exception &error, bool showDialog) noexcept
    {
        OutputDebugStringA("PhantomCore fatal error: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
        if (showDialog)
            MessageBoxA(nullptr, error.what(), "PhantomCore fatal error", MB_OK | MB_ICONERROR);
    }

    void AdvanceDeadline(
        double& deadline,
        const double interval,
        const double now) noexcept
    {
        deadline += interval;
        if (deadline < now - interval)
        {
            deadline = now + interval;
        }
    }

    struct MainLoopState final
    {
        MainLoopState(
            const EngineConfig& config,
            const double refreshRateHz)
            : renderInterval(
                  1.0 / ValidRate(
                      config.renderRateOverrideHz,
                      refreshRateHz)),
              audioInterval(
                  1.0 / ValidRate(config.audioUpdateRateHz, 500.0))
        {
        }

        HighResolutionClock clock;
        double totalSeconds{};
        std::uint64_t updateIndex{};
        std::uint64_t renderedFrames{};
        std::uint64_t updatesSinceStatisticsReport{};
        std::uint64_t rendersSinceStatisticsReport{};
        double statisticsReportStartSeconds{};
        PerformanceStatistics performance{};
        double renderInterval{};
        double nextRenderTime{};
        double audioInterval{};
        double nextAudioUpdateTime{};
    };

    void HandleWindowChanges(
        Win32Window& window,
        D3D12Renderer& renderer,
        IGameClient& client,
        const EngineConfig& config,
        MainLoopState& state)
    {
        WindowSize newSize{};
        if (window.ConsumeResize(newSize))
        {
            renderer.Resize(newSize.width, newSize.height);
            client.OnResize(newSize.width, newSize.height);
        }

        double newRefreshRate = 0.0;
        if (window.ConsumeRefreshRateChange(newRefreshRate) &&
            config.renderRateOverrideHz <= 1.0)
        {
            state.renderInterval =
                1.0 / ValidRate(newRefreshRate, 60.0);
            state.nextRenderTime = state.totalSeconds;
        }
    }

    [[nodiscard]] bool UpdateClient(
        IGameClient& client,
        const InputState& input,
        AudioSystem& audioSystem,
        const EngineConfig& config,
        const double rawDeltaSeconds,
        MainLoopState& state)
    {
        const UpdateContext updateContext{
            std::clamp(
                rawDeltaSeconds,
                0.0,
                config.maximumUpdateDeltaSeconds),
            state.totalSeconds,
            state.updateIndex++,
            input,
            audioSystem,
            state.performance};

        // Update is intentionally unthrottled. Rendering and FMOD use
        // independent deadlines and never sleep this loop.
        const bool keepRunning = client.Update(updateContext);
        ++state.updatesSinceStatisticsReport;
        return keepRunning;
    }

    void ServiceAudio(
        AudioSystem& audioSystem,
        MainLoopState& state)
    {
        if (state.totalSeconds < state.nextAudioUpdateTime)
        {
            return;
        }

        audioSystem.Update();
        AdvanceDeadline(
            state.nextAudioUpdateTime,
            state.audioInterval,
            state.totalSeconds);
    }

    [[nodiscard]] bool RenderIfDue(
        IGameClient& client,
        const Win32Window& window,
        D3D12Renderer& renderer,
        const EngineConfig& config,
        MainLoopState& state)
    {
        if (window.IsMinimized() ||
            state.totalSeconds < state.nextRenderTime)
        {
            return true;
        }

        // BeginFrame waits only when this back buffer is still in flight;
        // Client::Render records work into the opened command list.
        const RenderContext renderContext =
            renderer.BeginFrame(config.clearColor);
        client.Render(renderContext);
        renderer.EndFrame();

        ++state.renderedFrames;
        ++state.rendersSinceStatisticsReport;
        AdvanceDeadline(
            state.nextRenderTime,
            state.renderInterval,
            state.totalSeconds);

        return config.autoExitAfterRenderedFrames == 0 ||
            state.renderedFrames < config.autoExitAfterRenderedFrames;
    }

    void RefreshPerformanceStatistics(MainLoopState& state)
    {
        const double elapsedSeconds =
            state.totalSeconds - state.statisticsReportStartSeconds;
        if (elapsedSeconds < 1.0)
        {
            return;
        }

        state.performance.framesPerSecond =
            static_cast<std::uint64_t>(
                static_cast<double>(state.rendersSinceStatisticsReport) /
                    elapsedSeconds +
                0.5);
        state.performance.updatesPerSecond =
            static_cast<std::uint64_t>(
                static_cast<double>(state.updatesSinceStatisticsReport) /
                    elapsedSeconds +
                0.5);
        ++state.performance.measurementIndex;
        state.performance.hasMeasurement = true;
        state.updatesSinceStatisticsReport = 0;
        state.rendersSinceStatisticsReport = 0;
        state.statisticsReportStartSeconds = state.totalSeconds;
    }

    void RunMainLoop(
        IGameClient& client,
        InputState& input,
        Win32Window& window,
        D3D12Renderer& renderer,
        AudioSystem& audioSystem,
        const EngineConfig& config)
    {
        MainLoopState state(config, window.RefreshRateHz());
        bool running = true;
        while (running && window.PumpMessages())
        {
            // PumpMessages resets transient state and dispatches every
            // queued Raw Input message before the Client update.
            const double rawDeltaSeconds =
                state.clock.Tick(state.totalSeconds);
            HandleWindowChanges(window, renderer, client, config, state);
            running = UpdateClient(
                client,
                input,
                audioSystem,
                config,
                rawDeltaSeconds,
                state);
            ServiceAudio(audioSystem, state);
            if (running)
            {
                running = RenderIfDue(
                    client,
                    window,
                    renderer,
                    config,
                    state);
            }
            RefreshPerformanceStatistics(state);
        }
    }
}

static int RunClient(std::unique_ptr<IGameClient> client,
                     Microsoft::WRL::ComPtr<ID3D12InfoQueue>* diagnosticsQueue)
{
    if (client == nullptr)
    {
        return EXIT_FAILURE;
    }

    bool showErrorDialog = true;
    try
    {
        EngineConfig config = client->GetEngineConfig();
        showErrorDialog = config.showWindow;
        // The Win32 main thread owns the message pump and must remain an
        // STA so COM-based ASIO drivers can be created by FMOD.
        ComApartment comApartment(
            COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        // Stack order is intentional.  The Client is initialized only
        // after the native window, renderer, and audio service exist.
        D3D12Renderer renderer;
        AudioSystem audioSystem;
        InputState input;
        Win32Window window;
        bool clientInitialized = false;

        // Keep failure cleanup inside the subsystem scope. Entering the
        // outer handler unwinds these local services, so a Client cleanup
        // performed there would use renderer and audio references after
        // their owners had already been destroyed.
        try
        {
            window.Initialize(
                WindowConfig{
                    config.windowTitle,
                    config.windowWidth,
                    config.windowHeight,
                    config.showWindow},
                input);

            renderer.Initialize(
                window.Handle(),
                window.ClientWidth(),
                window.ClientHeight());

            if (diagnosticsQueue != nullptr)
            {
                static_cast<void>(renderer.Device()->QueryInterface(
                    IID_PPV_ARGS(diagnosticsQueue->GetAddressOf())));
            }

            config.audio.nativeWindowHandle = window.Handle();
            std::string audioError;
            if (!audioSystem.Initialize(
                    config.audio,
                    config.audioBackendFactory,
                    config.audioClipBackendFactory,
                    audioError))
            {
                throw std::runtime_error(
                    "Audio initialization failed: " + audioError);
            }

            const EngineServices services{renderer.MeshRendering(),
                                          renderer.TextRendering(),
                                          renderer.Visual2DRendering(),
                                          audioSystem,
                                          window.ClientWidth(),
                                          window.ClientHeight(),
                                          &window};
            // Client resource creation uses the high-level mesh/material
            // service; root signatures and PSOs remain owned by Graphics.
            client->Initialize(services);
            clientInitialized = true;

            RunMainLoop(
                *client,
                input,
                window,
                renderer,
                audioSystem,
                config);

            // Client scenes own D3D12 resources. First ensure no submitted
            // frame refers to them, then release Client resources while
            // the renderer/device and audio backend are still alive.
            renderer.WaitForGpu();
            client->Shutdown();
            clientInitialized = false;
            audioSystem.Shutdown();
            return EXIT_SUCCESS;
        }
        catch (...)
        {
            if (clientInitialized)
            {
                // Preserve the original failure if the device cannot wait.
                // Client cleanup must still happen before service teardown.
                try
                {
                    renderer.WaitForGpu();
                }
                catch (...)
                {
                }
                client->Shutdown();
                clientInitialized = false;
            }
            audioSystem.Shutdown();
            throw;
        }
    }
    catch (const std::exception& exception)
    {
        ReportFatalError(exception, showErrorDialog);
        return EXIT_FAILURE;
    }
}

int Run(std::unique_ptr<IGameClient> client)
{
    return RunClient(std::move(client), nullptr);
}

int RunWithGraphicsDiagnostics(std::unique_ptr<IGameClient> client, GraphicsDiagnostics& diagnostics)
{
    diagnostics = {};
    // Keep the backend queue alive through renderer and Client teardown.
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    const int exitCode = RunClient(std::move(client), &queue);
    if (queue)
        diagnostics = ReadGraphicsDiagnostics(*queue.Get());
    return exitCode;
}
