# Performance statistics

`Run` measures completed render frames, `IGameClient::Update` calls and the GPU
draw calls recorded into each rendered frame. It publishes a new one-second
sample through `UpdateContext::performance`.

| Field | Meaning |
| --- | --- |
| `framesPerSecond` | Rendered frames in the interval, rounded to whole frames per second |
| `updatesPerSecond` | `Update` calls in the interval, rounded per second |
| `drawCallsPerFrame` | Rounded average of draw calls recorded per rendered frame |
| `peakDrawCallsPerFrame` | Largest draw-call count of a single frame in the interval |
| `measurementIndex` | Increases once for each published sample |
| `hasMeasurement` | False until the first full interval is complete |

A draw call is one `DrawInstanced`/`DrawIndexedInstanced` recorded by the
mesh, text or Visual2D backend, including render-texture passes recorded while
the frame is open. Consecutive primitives that share a pipeline and texture
page are batched into one draw, so the count shows how much a screen's paint
order interleaves rectangles, images and text. It is a recording count, not a
GPU time measurement.

A Client can update its strings only when `measurementIndex` changes.

The Engine does not bind a key, load fonts, or draw an overlay. Those are
presentation decisions made by the Client. This keeps the engine usable by
games with different debug UIs, telemetry, or no visible counter at all.

`SceneGameClient` retains final scene-lifecycle methods and offers four safe
template hooks for root-client behavior:

- `OnClientInitialized` for Client-wide resource creation;
- `OnClientUpdated` for input and performance data;
- `OnClientRendered` after the active Scene has submitted its rendering;
- `OnClientShuttingDown` for release while Engine services are alive.

`PhantomCore.Collision.Tests --draw-statistics` runs a hidden engine with one
batched rectangle Canvas and checks that the first sample reports one draw per
frame.
