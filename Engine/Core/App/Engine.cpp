#include "App/Engine.h"
#include "App/MainLoopWatch.h"
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


namespace PhantomCore
{
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

    void Record(IEngineDiagnosticSink* sink, const std::string& line) noexcept
    {
        if (sink != nullptr)
            sink->OnEngineRecord(line);
    }

    [[nodiscard]] const char* Name(const WindowMode mode) noexcept
    {
        switch (mode)
        {
        case WindowMode::Windowed:
            return "Windowed";
        case WindowMode::Borderless:
            return "Borderless";
        }
        return "unknown";
    }

    [[nodiscard]] const char* Name(const RenderRateMode mode) noexcept
    {
        switch (mode)
        {
        case RenderRateMode::Display:
            return "Display";
        case RenderRateMode::DisplayDouble:
            return "DisplayDouble";
        case RenderRateMode::Unlimited:
            return "Unlimited";
        }
        return "unknown";
    }

    [[nodiscard]] std::string Size(const std::uint32_t width, const std::uint32_t height)
    {
        return std::to_string(width) + "x" + std::to_string(height);
    }

    [[nodiscard]] std::string Hertz(const double rate)
    {
        return std::to_string(static_cast<std::uint64_t>(std::max(rate, 0.0) + 0.5)) + " Hz";
    }

    // What the run starts with. Later changes are recorded one line each.
    void RecordStart(
        IEngineDiagnosticSink* sink,
        const D3D12Renderer& renderer,
        const Win32Window& window,
        const RenderRateMode renderRate)
    {
        if (sink == nullptr)
            return;
        for (const std::string& line : renderer.StartupRecord())
            sink->OnEngineRecord(line);
        Record(sink, std::string("window: ") + Size(window.ClientWidth(), window.ClientHeight()) + " " +
            Name(window.Mode()) + ", display " + Hertz(window.RefreshRateHz()) + ", render rate " + Name(renderRate));
    }

    // Called from a handler, while the renderer still exists: a lost device is
    // described before anything is released. The exception that revealed the
    // loss continues afterwards and ends the run as a fatal error.
    void ReportDeviceLoss(const D3D12Renderer& renderer, IEngineDiagnosticSink* sink) noexcept
    {
        if (sink == nullptr)
            return;
        try
        {
            const char* operation = nullptr;
            std::uint32_t resultCode = 0;
            try
            {
                throw;
            }
            catch (const GraphicsDeviceLostError& lost)
            {
                operation = lost.Operation();
                resultCode = lost.ResultCode();
            }
            catch (...)
            {
            }
            if (const auto report = renderer.DeviceLossReport(operation, resultCode))
                sink->OnGraphicsDeviceLost(*report);
        }
        catch (...)
        {
        }
    }

    // The window is destroyed while the exception unwinds, and that leaves a quit message in this thread's
    // queue. A message box that finds one closes at once, before anybody can read it. The run is over and
    // nothing is left to receive messages, so the queue is emptied before the error is shown.
    void DiscardQueuedMessages() noexcept
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
        }
    }

    void ReportFatalError(const std::exception &error, bool showDialog, IEngineDiagnosticSink* sink) noexcept
    {
        OutputDebugStringA("PhantomCore fatal error: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
        DiscardQueuedMessages();
        const bool handled =
            sink != nullptr && sink->OnFatalError(error.what()) == FatalErrorPresentation::Handled;
        if (showDialog && !handled)
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

    // The Client's requested mode. The loop copies it into the schedule at
    // the start of the next iteration, where the current time is known.
    class RenderRateService final : public IRenderRateControl
    {
    public:
        explicit RenderRateService(const RenderRateMode mode) noexcept
            : mode_(mode)
        {
        }

        [[nodiscard]] RenderRateMode Mode() const noexcept override
        {
            return mode_;
        }

        void SetMode(const RenderRateMode mode) noexcept override
        {
            mode_ = mode;
        }

    private:
        RenderRateMode mode_;
    };

    struct MainLoopState final
    {
        MainLoopState(
            const EngineConfig& config,
            const double refreshRateHz)
            : diagnosticSink(config.diagnostics.sink),
              recordedRefreshRate(Hertz(refreshRateHz)),
              recordedWindowMode(config.windowMode),
              recordedRenderRate(config.renderRateMode),
              audioInterval(
                  1.0 / ValidRate(config.audioUpdateRateHz, 500.0))
        {
            schedule.SetBaseRate(
                ValidRate(config.renderRateOverrideHz, refreshRateHz),
                0.0);
            schedule.SetMode(config.renderRateMode, 0.0);
            schedule.SetBackgroundRate(config.backgroundRenderRateHz);
        }

        HighResolutionClock clock;
        double totalSeconds{};
        std::uint64_t updateIndex{};
        std::uint64_t renderedFrames{};
        std::uint64_t updatesSinceStatisticsReport{};
        std::uint64_t rendersSinceStatisticsReport{};
        std::uint64_t drawCallsSinceStatisticsReport{};
        std::uint64_t peakDrawCallsSinceStatisticsReport{};
        double longestLoopSinceStatisticsReport{};
        double statisticsReportStartSeconds{};
        PerformanceStatistics performance{};
        // Diagnostics: the last values recorded, so only a change adds a line.
        IEngineDiagnosticSink* diagnosticSink{};
        std::string recordedRefreshRate;
        std::string recordedSize;
        WindowMode recordedWindowMode{};
        RenderRateMode recordedRenderRate{};
        // Background cap changes: recorded at most once per quiet period,
        // with the count of changes since the last line.
        bool lastBackgroundLimited{};
        std::uint64_t backgroundChangesSinceRecord{};
        double nextBackgroundRecordSeconds{};
        bool deviceRemovedForTest{};
        bool deviceHungForTest{};
        RenderSchedule schedule;
        double audioInterval{};
        double nextAudioUpdateTime{};
    };

    void RecordModeChanges(
        const Win32Window& window,
        const IRenderRateControl& renderRate,
        MainLoopState& state)
    {
        if (state.diagnosticSink == nullptr)
            return;
        if (window.Mode() != state.recordedWindowMode)
        {
            state.recordedWindowMode = window.Mode();
            Record(state.diagnosticSink, std::string("window mode: ") + Name(state.recordedWindowMode));
        }
        if (renderRate.Mode() != state.recordedRenderRate)
        {
            state.recordedRenderRate = renderRate.Mode();
            Record(state.diagnosticSink, std::string("render rate: ") + Name(state.recordedRenderRate));
        }
    }

    // Switching windows quickly must not flood the record: after a line, the
    // next one waits this long and then reports the state and the change count.
    constexpr double BackgroundRecordQuietSeconds = 10.0;

    void RecordBackgroundChanges(MainLoopState& state)
    {
        if (state.diagnosticSink == nullptr)
            return;
        const bool limited = state.schedule.IsBackgroundLimited();
        if (limited != state.lastBackgroundLimited)
        {
            state.lastBackgroundLimited = limited;
            ++state.backgroundChangesSinceRecord;
        }
        if (state.backgroundChangesSinceRecord == 0)
            return;
        if (state.totalSeconds < state.nextBackgroundRecordSeconds)
            return;
        std::string line = limited
            ? "render rate: background " + Hertz(1.0 / state.schedule.IntervalSeconds())
            : std::string("render rate: foreground");
        if (state.backgroundChangesSinceRecord > 1)
            line += " (" + std::to_string(state.backgroundChangesSinceRecord) + " focus changes)";
        Record(state.diagnosticSink, line);
        state.backgroundChangesSinceRecord = 0;
        state.nextBackgroundRecordSeconds = state.totalSeconds + BackgroundRecordQuietSeconds;
    }

    void HandleWindowChanges(
        Win32Window& window,
        D3D12Renderer& renderer,
        IGameClient& client,
        const EngineConfig& config,
        const IRenderRateControl& renderRate,
        MainLoopState& state)
    {
        WindowSize newSize{};
        if (window.ConsumeResize(newSize))
        {
            // The window reports its size once at start; only a new size is news.
            if (std::string size = Size(newSize.width, newSize.height); size != state.recordedSize)
            {
                Record(state.diagnosticSink, "window size: " + size);
                state.recordedSize = std::move(size);
            }
            renderer.Resize(newSize.width, newSize.height);
            client.OnResize(newSize.width, newSize.height);
        }

        double newRefreshRate = 0.0;
        if (window.ConsumeRefreshRateChange(newRefreshRate))
        {
            // The window reports its rate once at start; only a new value is news.
            if (std::string rate = Hertz(newRefreshRate); rate != state.recordedRefreshRate)
            {
                Record(state.diagnosticSink, "display: " + rate);
                state.recordedRefreshRate = std::move(rate);
            }
            if (config.renderRateOverrideHz <= 1.0)
            {
                state.schedule.SetBaseRate(
                    ValidRate(newRefreshRate, 60.0),
                    state.totalSeconds);
            }
        }
        state.schedule.SetMode(renderRate.Mode(), state.totalSeconds);
        // Only the Render calls slow down behind another program's window.
        state.schedule.SetBackground(window.IsInBackground(), state.totalSeconds);
        RecordModeChanges(window, renderRate, state);
        RecordBackgroundChanges(state);
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
        // independent deadlines and never sleep this loop. In the
        // Unlimited render mode every Update is followed by one Render.
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
            !state.schedule.IsDue(state.totalSeconds))
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
        const std::uint64_t drawCalls = renderer.LastFrameDrawCalls();
        state.drawCallsSinceStatisticsReport += drawCalls;
        state.peakDrawCallsSinceStatisticsReport =
            std::max(state.peakDrawCallsSinceStatisticsReport, drawCalls);
        state.schedule.MarkRendered(state.totalSeconds);

        return config.autoExitAfterRenderedFrames == 0 ||
            state.renderedFrames < config.autoExitAfterRenderedFrames;
    }

    // Test hook of EngineDiagnosticsConfig: loses the device once, on purpose.
    void RemoveDeviceIfRequested(
        D3D12Renderer& renderer,
        const EngineConfig& config,
        MainLoopState& state)
    {
        const std::uint64_t after = config.diagnostics.removeGraphicsDeviceAfterRenderedFrames;
        if (after == 0 || state.deviceRemovedForTest || state.renderedFrames < after)
            return;
        state.deviceRemovedForTest = true;
        renderer.RemoveDeviceForDiagnosticsTest();
    }

    // Test hook of EngineDiagnosticsConfig: one frame the GPU never finishes.
    void HangDeviceIfRequested(
        D3D12Renderer& renderer,
        const EngineConfig& config,
        MainLoopState& state)
    {
        const std::uint64_t after = config.diagnostics.hangGraphicsDeviceAfterRenderedFrames;
        if (after == 0 || state.deviceHungForTest || state.renderedFrames < after)
            return;
        state.deviceHungForTest = true;
        renderer.HangDeviceForDiagnosticsTest();
    }

    void RefreshPerformanceStatistics(const D3D12Renderer& renderer, MainLoopState& state)
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
        state.performance.drawCallsPerFrame =
            state.rendersSinceStatisticsReport == 0
                ? 0
                : (state.drawCallsSinceStatisticsReport + state.rendersSinceStatisticsReport / 2) /
                      state.rendersSinceStatisticsReport;
        state.performance.peakDrawCallsPerFrame = state.peakDrawCallsSinceStatisticsReport;
        state.performance.longestLoopSeconds = state.longestLoopSinceStatisticsReport;
        const GraphicsMemoryInfo memory = renderer.MemoryInfo();
        state.performance.graphicsMemoryUsageBytes = memory.usageBytes;
        state.performance.graphicsMemoryBudgetBytes = memory.budgetBytes;
        ++state.performance.measurementIndex;
        state.performance.hasMeasurement = true;
        state.updatesSinceStatisticsReport = 0;
        state.rendersSinceStatisticsReport = 0;
        state.drawCallsSinceStatisticsReport = 0;
        state.peakDrawCallsSinceStatisticsReport = 0;
        state.longestLoopSinceStatisticsReport = 0.0;
        state.statisticsReportStartSeconds = state.totalSeconds;
    }

    void RunMainLoop(
        IGameClient& client,
        InputState& input,
        Win32Window& window,
        D3D12Renderer& renderer,
        AudioSystem& audioSystem,
        const EngineConfig& config,
        const IRenderRateControl& renderRate)
    {
        MainLoopState state(config, window.RefreshRateHz());
        state.recordedSize = Size(window.ClientWidth(), window.ClientHeight());
        MainLoopWatch watch(window.Handle(), config.diagnostics.mainLoopStallSeconds, config.diagnostics.sink);
        bool running = true;
        while (running && window.PumpMessages())
        {
            watch.Beat();
            // PumpMessages resets transient state and dispatches every
            // queued Raw Input message before the Client update.
            const double rawDeltaSeconds =
                state.clock.Tick(state.totalSeconds);
            // The time since the previous Tick is the length of the previous pass.
            state.longestLoopSinceStatisticsReport =
                std::max(state.longestLoopSinceStatisticsReport, rawDeltaSeconds);
            HandleWindowChanges(
                window,
                renderer,
                client,
                config,
                renderRate,
                state);
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
                RemoveDeviceIfRequested(renderer, config, state);
                HangDeviceIfRequested(renderer, config, state);
            }
            RefreshPerformanceStatistics(renderer, state);
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
    IEngineDiagnosticSink* diagnosticSink = nullptr;
    try
    {
        EngineConfig config = client->GetEngineConfig();
        showErrorDialog = config.showWindow;
        diagnosticSink = config.diagnostics.sink;
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
        RenderRateService renderRate(config.renderRateMode);
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
                    config.showWindow,
                    config.windowMode,
                    config.windowPosition},
                input);

            renderer.Initialize(
                window.Handle(),
                window.ClientWidth(),
                window.ClientHeight(),
                config.diagnostics);
            RecordStart(diagnosticSink, renderer, window, config.renderRateMode);

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
                                          &window,
                                          &renderRate};
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
                config,
                renderRate);

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
            ReportDeviceLoss(renderer, diagnosticSink);
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
        ReportFatalError(exception, showErrorDialog, diagnosticSink);
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

} // namespace PhantomCore
