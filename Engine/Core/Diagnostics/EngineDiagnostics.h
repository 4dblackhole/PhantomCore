#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Diagnostics a Client can collect from one engine run. The engine decides
// what is worth recording; the Client decides where it goes (a log file, a
// crash folder, nothing). Nothing here opens a file or shows a window.
namespace PhantomCore
{
// Thrown by the graphics backend when a call fails because the device was
// removed, hung or reset. It stays a std::runtime_error with the usual
// "<operation> failed with HRESULT 0x........" text, so existing handlers
// keep working; handlers that care can tell a lost device from other failures.
class GraphicsDeviceLostError final : public std::runtime_error
{
public:
    // operation must have static storage (the backend passes string literals).
    GraphicsDeviceLostError(const char* operation, const std::uint32_t resultCode, const std::string& message)
        : std::runtime_error(message), operation_(operation), resultCode_(resultCode)
    {
    }

    [[nodiscard]] const char* Operation() const noexcept
    {
        return operation_;
    }
    [[nodiscard]] std::uint32_t ResultCode() const noexcept
    {
        return resultCode_;
    }

private:
    const char* operation_;
    std::uint32_t resultCode_;
};

// Graphics memory of this process. usageBytes is what it holds on the adapter,
// budgetBytes what Windows currently lets it hold before it starts moving
// resources out, and sharedUsageBytes what already lives in system memory.
struct GraphicsMemoryInfo
{
    bool available{};
    std::uint64_t usageBytes{};
    std::uint64_t budgetBytes{};
    std::uint64_t sharedUsageBytes{};
};

// What the engine knows at the moment the graphics device is found lost.
struct GraphicsDeviceLossReport
{
    // The engine call that failed and its result. Empty and zero when the
    // loss was found while another failure was being handled.
    std::string operation;
    std::uint32_t resultCode{};
    // The backend's reason for removing the device, as a number and a name
    // ("DXGI_ERROR_DEVICE_HUNG"); an unknown reason is named by its number.
    std::uint32_t removedReason{};
    std::string removedReasonName;
    // The last frame the renderer began, its back buffer and the draw calls
    // of the last frame it completed.
    std::uint64_t renderIndex{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t lastFrameDrawCalls{};
    GraphicsMemoryInfo memory;
    // EngineDiagnosticsConfig::graphicsDeviceRemovedData. When it was on,
    // extendedData holds one line per command list (its last completed
    // operation) and per allocation near a page fault, or a line saying that
    // the backend kept none. It is empty only when the data was off.
    bool extendedDataEnabled{};
    std::vector<std::string> extendedData;
};

// The main loop has not finished a pass for stalledSeconds, and its thread
// is not running a window loop of its own (moving or sizing the window, a
// menu, a modal dialog). loopPasses counts the passes finished before that.
struct MainLoopStallReport
{
    double stalledSeconds{};
    std::uint32_t mainThreadId{};
    std::uint64_t loopPasses{};
};

// Who tells the user about a fatal error.
enum class FatalErrorPresentation : std::uint8_t
{
    EngineDialog, // the engine shows its own message box (not in hidden runs)
    Handled,      // the Client presented the error itself
};

// Receives diagnostics from Run. None of the calls may throw, and the engine
// never owns the sink. The calls come from the thread that called Run and must
// return quickly, except the two about a stalled main loop: that thread is the
// one that is stuck, so they come from the engine's watch thread.
class IEngineDiagnosticSink
{
public:
    // One line of plain ASCII text without a line break: the adapter and its
    // driver, the swap chain, the window and render rate at start, and each
    // later change of those.
    virtual void OnEngineRecord(std::string_view line) noexcept = 0;
    // Called once, before the engine releases anything, when the graphics
    // device is lost. The failure then continues as a fatal error.
    virtual void OnGraphicsDeviceLost(const GraphicsDeviceLossReport& report) noexcept = 0;
    // Called once with the text of the exception that ends the run, after
    // the Client was shut down and the engine services were released.
    [[nodiscard]] virtual FatalErrorPresentation OnFatalError(std::string_view message) noexcept = 0;
    // From the watch thread, once per stall, while the main loop is still
    // stuck: the moment to write a dump of where it waits. Nothing the main
    // thread owns may be touched without the Client's own synchronization.
    virtual void OnMainLoopStalled(const MainLoopStallReport& report) noexcept = 0;
    // From the watch thread, when a reported stall ended after all.
    virtual void OnMainLoopResumed(double stalledSeconds) noexcept = 0;

protected:
    ~IEngineDiagnosticSink() = default;
};

struct EngineDiagnosticsConfig
{
    // Must outlive Run. Null records nothing.
    IEngineDiagnosticSink* sink{};
    // Asks the graphics backend to keep, for every command list, which
    // operation the GPU completed last, and what was allocated around a page
    // fault (D3D12 DRED). It costs a little on every draw, so it is off unless
    // a crash is being chased. It cannot change after the device exists.
    bool graphicsDeviceRemovedData{};
    // A main loop pass that takes longer than this is reported as a stall.
    // Zero or less does not watch. A value well above any honest pause (a
    // synchronous load) keeps the report for real hangs. A debugger and the
    // window's own loops (moving, sizing, menus, dialogs) are never reported.
    double mainLoopStallSeconds{};
    // Test hook: removes the graphics device after this many rendered frames
    // so the loss path can be exercised on purpose. Zero never does.
    std::uint64_t removeGraphicsDeviceAfterRenderedFrames{};
};

} // namespace PhantomCore
