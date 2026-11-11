#include "../Audio/AudioPcmTests.h"
#include "../App/HeadlessRunTests.h"
#include "../App/ImageReadbackTests.h"
#include "../App/SceneManagerTests.h"
#include "../App/Visual2DPixelTests.h"
#include "../App/TextPixelTests.h"
#include "../App/TextRenderingTests.h"
#include "../App/WindowControlTests.h"
#include "../App/InputStateTests.h"
#include "../App/TextureLifetimeTests.h"
#include "PhantomCore.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string_view>
#include <utility>

using namespace PhantomCore;

namespace
{
    int failureCount = 0;

    void Check(const bool condition, const std::string_view description)
    {
        if (!condition)
        {
            ++failureCount;
            std::cerr << "FAILED: " << description << '\n';
        }
    }

    [[nodiscard]] bool NearlyEqual(
        const float first,
        const float second,
        const float epsilon = 1.0e-4F) noexcept
    {
        return std::abs(first - second) <= epsilon;
    }

    struct PlaybackProbe
    {
        bool playing{true};
        bool paused{};
        bool failStop{};
        bool finishOnRestart{};
        bool failRestart{};
        int stops{};
        int restarts{};
        std::uint64_t startDspClock{};
        std::uint64_t startPositionMicroseconds{};
        IAudioBusBackend* restartBus{};
    };
    std::vector<std::shared_ptr<PlaybackProbe>> playbackProbes;
    bool failTestPlayback{};
    AudioClipLoadState testClipState{AudioClipLoadState::Ready};

    class TestVisual2DRenderer final :
        public Visual2DRenderSystem
    {
    public:
        [[nodiscard]] ImageHandle LoadImage(
            const std::filesystem::path&) override
        {
            ++imageLoads;
            return {static_cast<std::uint64_t>(imageLoads)};
        }

        [[nodiscard]] Size GetImageSize(
            const ImageHandle image) const noexcept override
        {
            return image ? Size{64.0F, 32.0F}
                         : Size{};
        }

        void SubmitScreen(
            const Visual2DCanvas& canvas,
            const RenderContext&,
            const Point screenOrigin,
            const std::uint32_t canvasZOrder) override
        {
            ++screenSubmissions;
            lastCanvas = &canvas;
            lastOrigin = screenOrigin;
            lastZOrder = canvasZOrder;
        }

        void SubmitPlane(
            const Visual2DCanvas&,
            const RenderContext&,
            const DirectX::XMFLOAT4X4&,
            Size,
            const DirectX::XMFLOAT4X4&) override
        {
        }

        [[nodiscard]] RenderTargetTextureHandle
            CreateCanvasRenderTarget(std::uint32_t, std::uint32_t) override
        {
            return {};
        }

        void RenderToTexture(
            const Visual2DCanvas&,
            const RenderTargetTextureHandle&,
            const RenderContext&) override
        {
        }

        int imageLoads{};
        int screenSubmissions{};
        const Visual2DCanvas* lastCanvas{};
        Point lastOrigin{};
        std::uint32_t lastZOrder{};
    };

    void TestManagedScreenVisual2D()
    {


        TestVisual2DRenderer renderer;
        ScreenVisual2DManager manager;
        manager.Initialize(renderer, {1280.0F, 720.0F});
        ScreenCanvasHandle canvasHandle = manager.CreateOwnedCanvas({
            {1280.0F, 720.0F},
            CanvasScaleMode::FixedHeight,
            {12.0F, 24.0F},
            3,
            false});
        const ScreenCanvasId canvasId = canvasHandle.Id();
        Visual2DCanvas* const canvas = canvasHandle.Get();
        Check(canvas != nullptr && manager.CanvasCount() == 1,
            "screen presentation manager owns created Canvas trees");
        if (canvas == nullptr)
        {
            return;
        }

        const ImageHandle first = manager.RegisterImage("images/test.png");
        const ImageHandle duplicate = manager.RegisterImage(
            "IMAGES/folder/../TEST.PNG");
        Check(first.value == duplicate.value && renderer.imageLoads == 1 &&
            manager.ImageCount() == 1,
            "screen presentation manager caches normalized image paths");
        Check(NearlyEqual(manager.GetImageSize(first).width, 64.0F),
            "screen presentation manager exposes registered image metadata");

        RenderContext context;
        manager.Render(context);
        Check(renderer.screenSubmissions == 0,
            "hidden managed Canvas is not submitted");
        Check(canvasHandle.SetVisible(true),
            "managed Canvas visibility can be enabled");
        manager.Render(context);
        Check(renderer.screenSubmissions == 1 &&
            renderer.lastCanvas == canvas &&
            NearlyEqual(renderer.lastOrigin.x, 12.0F) &&
            renderer.lastZOrder == 3,
            "manager submits visible Canvas with its placement and Z-order");

        manager.OnResize(1920, 1080);
        Check(NearlyEqual(canvas->ViewportSize().width, 1920.0F) &&
            NearlyEqual(canvas->PixelScale(), 1.5F),
            "manager resizes owned Canvas trees with the Client viewport");
        ScreenCanvasHandle movedHandle = std::move(canvasHandle);
        Check(movedHandle.Get() == canvas && manager.CanvasCount() == 1,
            "moving a Canvas handle transfers ownership without removing it");
        canvasHandle.Reset();
        Check(!canvasHandle && movedHandle.Get() == canvas &&
            manager.CanvasCount() == 1,
            "resetting a moved-from handle preserves transferred Canvas ownership");
        movedHandle.Reset();
        Check(manager.CanvasCount() == 0 &&
            manager.FindCanvas(canvasId) == nullptr,
            "resetting an owned Canvas handle removes its presentation");
        {
            ScreenCanvasHandle automatic = manager.CreateOwnedCanvas();
            Check(automatic && manager.CanvasCount() == 1,
                "an owned Canvas remains registered for its handle lifetime");
        }
        Check(manager.CanvasCount() == 0,
            "destroying an owned Canvas handle removes its presentation");
        manager.Shutdown();
        Check(manager.ImageCount() == 0,
            "manager shutdown clears game-facing image registrations");
    }

    class TestVoice final : public IAudioVoiceBackend
    {
    public:
        explicit TestVoice(std::shared_ptr<PlaybackProbe> probe)
            : probe_(std::move(probe)) {}
        bool IsPlaying() const noexcept override { return probe_->playing; }
        bool Restart(
            const AudioPlaybackSettings& settings,
            IAudioBusBackend* bus,
            std::string& error) override
        {
            if (probe_->finishOnRestart)
            {
                probe_->playing = false;
                error = "ended during restart";
                return false;
            }
            if (probe_->failRestart)
            {
                error = "active restart failure";
                return false;
            }
            ++probe_->restarts;
            probe_->playing = true;
            probe_->paused = settings.startPaused;
            probe_->startDspClock = settings.startDspClock;
            probe_->startPositionMicroseconds = settings.startPositionMicroseconds;
            probe_->restartBus = bus;
            return true;
        }
        bool Stop(std::string& error) override
        {
            if (probe_->failStop)
            {
                error = "test stop failure";
                return false;
            }
            ++probe_->stops;
            probe_->playing = false;
            return true;
        }
        bool SetPaused(bool paused, std::string&) override
        {
            probe_->paused = paused;
            return true;
        }
        bool SetVolume(float, std::string&) override { return true; }
        bool SetPitch(float, std::string&) override { return true; }
        bool SetDelay(std::uint64_t start, std::uint64_t, std::string&) override
        { probe_->startDspClock = start; return true; }
    private:
        std::shared_ptr<PlaybackProbe> probe_;
    };

    class TestClip final : public IAudioClipBackend
    {
    public:
        bool GetDurationMicroseconds(std::uint64_t& duration, std::string&) const override
        { duration = 1'000'000; return true; }
        AudioClipLoadState LoadState(std::string&) const override { return testClipState; }
        std::unique_ptr<IAudioVoiceBackend> Play(
            const AudioPlaybackSettings& settings,
            IAudioBusBackend*, std::string& error) override
        {
            if (failTestPlayback)
            {
                error = "test playback failure";
                return nullptr;
            }
            auto probe = std::make_shared<PlaybackProbe>();
            probe->paused = settings.startPaused;
            probe->startDspClock = settings.startDspClock;
            probe->startPositionMicroseconds = settings.startPositionMicroseconds;
            playbackProbes.push_back(probe);
            return std::make_unique<TestVoice>(std::move(probe));
        }
    };

    std::unique_ptr<IAudioClipBackend> CreateTestClip(
        IAudioBackend&, const std::filesystem::path&,
        AudioLoadMode, std::string&)
    {
        return std::make_unique<TestClip>();
    }

    void TestManagedAudioPlayback()
    {

        AudioSystem audio;
        AudioConfig config;
        config.preferredBackend = AudioOutputBackend::NoSound;
        std::string error;
        const bool initialized = audio.Initialize(
            config, nullptr, &CreateTestClip, error);
        Check(initialized, "audio test initializes without an output device");
        if (!initialized) { return; }

        AudioPlaybackManager manager;
        Check(manager.Play(nullptr, {}, nullptr, error) == InvalidAudioPlaybackId,
            "managed playback rejects null clips");
        std::shared_ptr<AudioClip> clip = audio.LoadSound("probe", error);
        std::shared_ptr<AudioBus> bus = audio.CreateBus("probe", nullptr, error);
        Check(clip != nullptr && bus != nullptr, "audio test creates resources");
        if (clip == nullptr || bus == nullptr) { return; }
        std::uint64_t duration = 0;
        Check(clip->GetDurationMicroseconds(duration, error) && duration == 1'000'000,
            "clip forwards source duration in microseconds");
        const std::weak_ptr<AudioClip> weakClip = clip;
        const std::weak_ptr<AudioBus> weakBus = bus;
        AudioPlaybackSettings settings;
        settings.startPaused = true;
        settings.startDspClock = 123456;
        settings.startPositionMicroseconds = 456789;
        const auto first = manager.Play(clip, settings, bus, error);
        const auto second = manager.Play(clip, {}, bus, error);
        Check(first != 0 && second != 0 && first != second,
            "overlapping playback has independent IDs");
        if (first == 0 || second == 0) { return; }
        clip.reset();
        bus.reset();
        manager.Update();
        Check(!weakClip.expired() && !weakBus.expired() &&
            manager.PlaybackCount() == 2 && playbackProbes[0]->paused &&
            playbackProbes[0]->startDspClock == 123456,
            "manager retains assets and paused/scheduled voices after owner releases them");
        Check(playbackProbes[0]->startPositionMicroseconds == 456789 &&
            manager.FindVoice(first)->SetDelay(222222, 0, error) &&
            playbackProbes[0]->startDspClock == 222222,
            "source seek and prepared-voice DSP start are forwarded independently");
        Check(manager.FindVoice(first)->SetPaused(false, error) &&
            !playbackProbes[0]->paused, "managed voice supports resume");
        std::shared_ptr<AudioBus> restartBus =
            audio.CreateBus("restart", nullptr, error);
        const std::weak_ptr<AudioBus> weakRestartBus = restartBus;
        AudioPlaybackSettings restartSettings;
        restartSettings.startPaused = true;
        restartSettings.startDspClock = 654321;
        restartSettings.startPositionMicroseconds = 1234;
        Check(manager.Restart(
                first, restartSettings, restartBus, error) &&
            manager.PlaybackCount() == 2 &&
            playbackProbes.size() == 2 &&
            playbackProbes[0]->restarts == 1 &&
            playbackProbes[0]->paused &&
            playbackProbes[0]->startDspClock == 654321 &&
            playbackProbes[0]->startPositionMicroseconds == 1234 &&
            playbackProbes[0]->restartBus != nullptr,
            "managed playback restarts its existing voice without allocating another one");
        restartBus.reset();
        Check(!weakRestartBus.expired(),
            "restarted playback retains its replacement bus");
        playbackProbes[0]->failStop = true;
        Check(!manager.Stop(first, error) && manager.FindVoice(first) != nullptr,
            "failed stop keeps the voice controllable");
        playbackProbes[0]->failStop = false;
        Check(manager.Stop(first, error) && playbackProbes[0]->stops == 1 &&
            playbackProbes[1]->playing && manager.FindVoice(first) == nullptr,
            "stopping one playback explicitly stops only that voice");
        Check(weakRestartBus.expired(),
            "stopping restarted playback releases its replacement bus");
        Check(!manager.Restart(first, {}, nullptr, error),
            "stale playback IDs cannot restart another voice");
        Check(!manager.Stop(first, error), "stale playback IDs cannot stop another voice");
        playbackProbes[1]->playing = false;
        manager.Update();
        Check(manager.PlaybackCount() == 0 && weakClip.expired() && weakBus.expired(),
            "natural completion releases manager resources");

        clip = audio.LoadSound("probe", error);
        failTestPlayback = true;
        Check(manager.Play(clip, {}, nullptr, error) == 0 &&
            manager.PlaybackCount() == 0 && error == "test playback failure",
            "failed playback leaves no entry and preserves the backend error");
        failTestPlayback = false;
        const auto third = manager.Play(clip, {}, nullptr, error);
        manager.StopAll();
        const auto fourth = manager.Play(clip, {}, nullptr, error);
        Check(third > second && fourth > third && playbackProbes[2]->stops == 1,
            "StopAll explicitly stops voices without reusing IDs");
        manager.StopAll();
        Check(manager.PlaybackCount() == 0 && playbackProbes[3]->stops == 1,
            "shutdown clears remaining managed playback");
        {
            AudioPlaybackManager temporary;
            Check(temporary.Play(clip, {}, nullptr, error) != 0,
                "temporary manager starts playback");
        }
        Check(playbackProbes.back()->stops == 1,
            "manager destructor explicitly stops playback");
        const auto recovered = manager.Play(clip, {}, nullptr, error);
        const auto ended = playbackProbes.back();
        ended->finishOnRestart = true;
        Check(manager.Restart(recovered, settings, nullptr, error) &&
                  manager.PlaybackCount() == 1 && playbackProbes.back() != ended && error.empty(),
              "a mixer ending the channel during Restart recreates a voice under the same ID");
        const auto live = playbackProbes.back();
        live->failRestart = true;
        Check(!manager.Restart(recovered, settings, nullptr, error) &&
                  error == "active restart failure" && playbackProbes.back() == live,
              "an error on a live channel is preserved rather than hidden by replacement");
        manager.StopAll();
        playbackProbes.clear();
        std::shared_ptr<AudioClip> retired = audio.LoadSound("retired", error);
        const std::weak_ptr<AudioClip> weakRetired = retired;
        testClipState = AudioClipLoadState::Loading;
        audio.RetireClip(std::move(retired));
        audio.Update();
        Check(!weakRetired.expired(), "retired loading clips survive until native I/O settles");
        testClipState = AudioClipLoadState::Ready;
        audio.Update();
        Check(weakRetired.expired(), "settled retired clips release on the audio update");
    }

    [[nodiscard]] bool MatricesNearlyEqual(
        const DirectX::XMMATRIX first,
        const DirectX::XMMATRIX second,
        const float epsilon = 1.0e-4F) noexcept
    {
        const DirectX::XMVECTOR tolerance =
            DirectX::XMVectorReplicate(epsilon);
        return DirectX::XMVector4NearEqual(first.r[0], second.r[0], tolerance) &&
            DirectX::XMVector4NearEqual(first.r[1], second.r[1], tolerance) &&
            DirectX::XMVector4NearEqual(first.r[2], second.r[2], tolerance) &&
            DirectX::XMVector4NearEqual(first.r[3], second.r[3], tolerance);
    }

    void TestCameraMatrices()
    {
        using namespace DirectX;

        Camera camera;
        const Camera& readOnlyCamera = camera;
        const XMMATRIX initialView = readOnlyCamera.ViewMatrix();
        const XMMATRIX initialProjection = readOnlyCamera.ProjectionMatrix();
        Check(
            MatricesNearlyEqual(
                readOnlyCamera.ViewProjectionMatrix(),
                initialView * initialProjection),
            "a const Camera returns its cached view-projection matrix");

        camera.SetPosition(2.0F, 3.0F, -4.0F);
        camera.SetYawPitchRadians(XM_PIDIV4, -0.2F);
        const XMMATRIX movedView = readOnlyCamera.ViewMatrix();
        Check(
            !MatricesNearlyEqual(movedView, initialView),
            "camera movement invalidates the cached view matrix");
        Check(
            MatricesNearlyEqual(
                readOnlyCamera.ProjectionMatrix(),
                initialProjection),
            "camera movement preserves the cached projection matrix");
        Check(
            MatricesNearlyEqual(
                readOnlyCamera.ViewProjectionMatrix(),
                movedView * initialProjection),
            "camera movement refreshes the cached view-projection matrix");

        camera.SetPerspective(XM_PIDIV2, 16.0F / 9.0F, 0.5F, 500.0F);
        const XMMATRIX perspectiveProjection = readOnlyCamera.ProjectionMatrix();
        Check(
            MatricesNearlyEqual(readOnlyCamera.ViewMatrix(), movedView),
            "projection changes preserve the cached view matrix");
        Check(
            !MatricesNearlyEqual(perspectiveProjection, initialProjection),
            "perspective changes invalidate the cached projection matrix");
        Check(
            MatricesNearlyEqual(
                readOnlyCamera.ViewProjectionMatrix(),
                movedView * perspectiveProjection),
            "perspective changes refresh the cached view-projection matrix");

        camera.SetOrthographic(1920.0F, 1080.0F, 0.0F, 10.0F);
        const XMMATRIX orthographicProjection =
            XMMatrixOrthographicLH(1920.0F, 1080.0F, 0.0F, 10.0F);
        Check(
            MatricesNearlyEqual(
                readOnlyCamera.ProjectionMatrix(),
                orthographicProjection),
            "orthographic mode refreshes the cached projection matrix");
        Check(
            MatricesNearlyEqual(
                readOnlyCamera.ViewProjectionMatrix(),
                movedView * orthographicProjection),
            "orthographic mode refreshes the cached view-projection matrix");
    }

    void TestPerspectiveFrustum()
    {


        Camera camera;
        camera.SetPerspective(DirectX::XM_PIDIV2, 1.0F, 1.0F, 10.0F);
        const ViewFrustum& frustum = camera.Frustum();

        Check(
            Classify(frustum, Sphere3D{{0.0F, 0.0F, 5.0F}, 0.5F}) ==
                VolumeIntersection::Inside,
            "perspective frustum contains a sphere in front of the camera");
        Check(
            Classify(frustum, Sphere3D{{0.0F, 0.0F, -2.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "perspective frustum rejects a sphere behind the camera");
        Check(
            Classify(frustum, Sphere3D{{-6.0F, 0.0F, 5.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "perspective frustum rejects a sphere beyond the left plane");
        Check(
            Classify(frustum, Sphere3D{{6.0F, 0.0F, 5.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "perspective frustum rejects a sphere beyond the right plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, 6.0F, 5.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "perspective frustum rejects a sphere beyond the top plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, -6.0F, 5.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "perspective frustum rejects a sphere beyond the bottom plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, 0.0F, 0.25F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "perspective frustum rejects a sphere before the near plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, 0.0F, 11.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "perspective frustum rejects a sphere beyond the far plane");
        Check(
            Classify(frustum, Sphere3D{{5.2F, 0.0F, 5.0F}, 0.5F}) ==
                VolumeIntersection::Intersecting,
            "perspective frustum reports a sphere crossing a side plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, 0.0F, 0.75F}, 0.5F}) ==
                VolumeIntersection::Intersecting,
            "perspective frustum reports a sphere crossing the near plane");
    }

    void TestOrthographicFrustum()
    {


        Camera camera;
        camera.SetOrthographic(8.0F, 6.0F, 1.0F, 10.0F);
        const ViewFrustum& frustum = camera.Frustum();

        Check(
            Classify(frustum, Sphere3D{{0.0F, 0.0F, 5.0F}, 0.5F}) ==
                VolumeIntersection::Inside,
            "orthographic frustum contains a centered sphere");
        Check(
            Classify(frustum, Sphere3D{{-4.5F, 0.0F, 5.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "orthographic frustum rejects a sphere beyond the left plane");
        Check(
            Classify(frustum, Sphere3D{{4.5F, 0.0F, 5.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "orthographic frustum rejects a sphere beyond the right plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, 3.5F, 5.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "orthographic frustum rejects a sphere beyond the top plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, -3.5F, 5.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "orthographic frustum rejects a sphere beyond the bottom plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, 0.0F, 0.25F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "orthographic frustum rejects a sphere before the near plane");
        Check(
            Classify(frustum, Sphere3D{{0.0F, 0.0F, 11.0F}, 0.25F}) ==
                VolumeIntersection::Outside,
            "orthographic frustum rejects a sphere beyond the far plane");
        Check(
            Classify(frustum, Sphere3D{{4.2F, 0.0F, 5.0F}, 0.5F}) ==
                VolumeIntersection::Intersecting,
            "orthographic frustum reports a sphere crossing a side plane");

        const Sphere3D projectionProbe{{3.0F, 0.0F, 5.0F}, 0.25F};
        Check(
            Classify(frustum, projectionProbe) == VolumeIntersection::Inside,
            "orthographic probe starts inside the cached frustum");
        camera.SetOrthographicSize(4.0F, 6.0F);
        Check(
            Classify(camera.Frustum(), projectionProbe) ==
                VolumeIntersection::Outside,
            "projection changes refresh the cached camera frustum");
    }

    void TestFrustumAndWorldBoundsUpdates()
    {


        Camera camera;
        camera.SetOrthographic(10.0F, 10.0F, 1.0F, 20.0F);
        const Sphere3D localSphere{{0.0F, 0.0F, 0.0F}, 1.0F};

        TransformNode parent;
        parent.SetPosition(3.0F, 2.0F, 5.0F);
        parent.SetScale(2.0F, 3.0F, 4.0F);

        TransformNode instanceTransform;
        instanceTransform.SetParent(&parent);
        instanceTransform.SetPosition(1.0F, 0.0F, 0.0F);
        instanceTransform.SetRotationRollPitchYaw(
            0.0F,
            DirectX::XM_PIDIV4,
            0.0F);

        const Sphere3D initialWorldSphere = TransformSphere(
            localSphere,
            instanceTransform.WorldMatrix());
        Check(
            NearlyEqual(initialWorldSphere.center.x, 5.0F) &&
                NearlyEqual(initialWorldSphere.center.y, 2.0F) &&
                NearlyEqual(initialWorldSphere.center.z, 5.0F),
            "world sphere center follows the parent transform");
        Check(
            initialWorldSphere.radius >= 4.0F,
            "world sphere conservatively covers rotation and nonuniform scale");
        Check(
            Classify(camera.Frustum(), initialWorldSphere) !=
                VolumeIntersection::Outside,
            "transformed instance bounds remain visible when crossing a plane");

        parent.SetPosition(20.0F, 2.0F, 5.0F);
        const Sphere3D movedWorldSphere = TransformSphere(
            localSphere,
            instanceTransform.WorldMatrix());
        Check(
            movedWorldSphere.center.x > initialWorldSphere.center.x + 10.0F,
            "world sphere refreshes after its parent moves");
        Check(
            Classify(camera.Frustum(), movedWorldSphere) ==
                VolumeIntersection::Outside,
            "moved instance bounds leave the cached camera frustum");

        camera.SetPosition(20.0F, 0.0F, 0.0F);
        Check(
            Classify(camera.Frustum(), movedWorldSphere) !=
                VolumeIntersection::Outside,
            "camera movement refreshes the frustum around moved bounds");

        DirectX::XMFLOAT4X4 rotatedNonuniformWorld{};
        DirectX::XMStoreFloat4x4(
            &rotatedNonuniformWorld,
            DirectX::XMMatrixScaling(-2.0F, 3.0F, 4.0F) *
                DirectX::XMMatrixRotationY(DirectX::XM_PIDIV4));
        const Sphere3D rotatedWorldSphere = TransformSphere(
            Sphere3D{{1.0F, 0.0F, 0.0F}, 2.0F},
            rotatedNonuniformWorld);
        Check(
            NearlyEqual(rotatedWorldSphere.radius, 8.0F),
            "rotated sphere radius uses the largest absolute axis scale");
    }

    void TestPlaneAndLines()
    {


        const Plane3D ground{{0.0F, 2.0F, 0.0F}, 0.0F};
        const Line3D descending{{0.0F, 2.0F, 0.0F}, {0.0F, -2.0F, 0.0F}};
        const auto lineHit = Intersect(ground, descending);
        Check(lineHit.has_value(), "plane intersects an infinite line");
        if (lineHit)
        {
            Check(NearlyEqual(lineHit->parameter, 1.0F), "line parameter");
            Check(NearlyEqual(lineHit->point.y, 0.0F), "line hit position");
            Check(NearlyEqual(lineHit->normal.y, 1.0F), "plane normal normalized");
        }

        Check(
            !Intersects(
                ground,
                Line3D{{0.0F, 2.0F, 0.0F}, {1.0F, 0.0F, 0.0F}}),
            "separate parallel line does not intersect plane");
        Check(
            Intersects(
                ground,
                Line3D{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}}),
            "coplanar line intersects plane");
        Check(
            !Intersects(
                ground,
                Ray3D{{0.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}}),
            "ray pointing away from plane does not intersect");

        const auto segmentHit = Intersect(
            ground,
            LineSegment3D{{0.0F, -1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}});
        Check(segmentHit.has_value(), "plane intersects finite segment");
        if (segmentHit)
        {
            Check(
                NearlyEqual(segmentHit->parameter, 0.5F),
                "segment parameter is normalized");
        }
        Check(
            Intersects(
                ground,
                LineSegment3D{{1.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F}}),
            "degenerate segment on plane is a point intersection");
    }

    void TestTwoDimensionalQueries()
    {


        const Circle2D circle{{0.0F, 1.0F}, 1.0F};
        Check(
            Intersects(circle, Line2D{{0.0F, 0.0F}, {2.0F, 0.0F}}),
            "circle tangent to infinite line");
        Check(
            !Intersects(circle, Line2D{{0.0F, -1.0F}, {1.0F, 0.0F}}),
            "circle separate from infinite line");
        Check(
            Intersects(
                circle,
                LineSegment2D{{-2.0F, 0.0F}, {2.0F, 0.0F}}),
            "circle tangent to segment");
        Check(
            !Intersects(
                circle,
                LineSegment2D{{2.0F, 0.0F}, {3.0F, 0.0F}}),
            "circle does not reach finite segment");

        const Obb2D rotatedBox{
            {0.0F, 0.0F},
            {2.0F, 0.5F},
            DirectX::XM_PIDIV4};
        Check(
            Intersects(Circle2D{{1.2F, 1.2F}, 0.25F}, rotatedBox),
            "circle intersects rotated OBB");
        Check(
            !Intersects(Circle2D{{4.0F, 4.0F}, 0.5F}, rotatedBox),
            "circle is separate from rotated OBB");

        const Obb2D overlapping{
            {1.0F, 0.0F},
            {1.0F, 1.0F},
            -DirectX::XM_PIDIV4};
        const Obb2D separate{
            {6.0F, 0.0F},
            {1.0F, 1.0F},
            DirectX::XM_PIDIV4};
        Check(Intersects(rotatedBox, overlapping), "two OBBs overlap");
        Check(!Intersects(rotatedBox, separate), "two OBBs are separate");

        const DirectX::XMFLOAT2 closest = ClosestPoint(
            Obb2D{{0.0F, 0.0F}, {1.0F, 2.0F}, 0.0F},
            {4.0F, 3.0F});
        Check(
            NearlyEqual(closest.x, 1.0F) && NearlyEqual(closest.y, 2.0F),
            "closest point on 2D OBB");

        Check(
            !Intersects(
                Circle2D{{0.0F, 0.0F}, -1.0F},
                Line2D{{}, {1.0F, 0.0F}}),
            "negative circle radius is rejected");
        Check(
            !Intersects(
                Circle2D{{0.0F, 0.0F}, 1.0F},
                Line2D{{}, {0.0F, 0.0F}}),
            "zero line direction is rejected");
    }

    void TestThreeDimensionalVolumes()
    {


        const Obb3D first{
            {0.0F, 0.0F, 0.0F},
            {1.0F, 1.0F, 1.0F},
            {0.0F, 0.0F, 0.0F, 1.0F}};
        const float halfAngle = DirectX::XM_PIDIV4 * 0.5F;
        const Obb3D rotated{
            {1.4F, 0.0F, 0.0F},
            {1.0F, 0.5F, 1.0F},
            {0.0F, std::sin(halfAngle), 0.0F, std::cos(halfAngle)}};
        const Obb3D separate{
            {5.0F, 0.0F, 0.0F},
            {1.0F, 1.0F, 1.0F},
            {0.0F, 0.0F, 0.0F, 1.0F}};

        Check(Intersects(first, rotated), "two 3D OBBs overlap");
        Check(!Intersects(first, separate), "two 3D OBBs are separate");

        const Sphere3D sphere{{0.0F, 0.0F, 0.0F}, 1.0F};
        Check(
            Intersects(
                sphere,
                Line3D{{-2.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}}),
            "sphere tangent to infinite line");
        Check(
            !Intersects(
                sphere,
                Ray3D{{2.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}}),
            "ray points away from sphere");
        Check(
            Intersects(
                sphere,
                LineSegment3D{{-2.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}}),
            "sphere intersects finite segment");
        Check(Intersects(sphere, first), "sphere intersects 3D OBB");
        Check(
            !Intersects(Sphere3D{{4.0F, 4.0F, 4.0F}, 0.5F}, first),
            "sphere is separate from 3D OBB");
    }

    void TestTriangleAndUvSurfaces()
    {


        const Triangle3D triangle{
            {-1.0F, -1.0F, 0.0F},
            {-1.0F, 1.0F, 0.0F},
            {1.0F, 1.0F, 0.0F}};
        const Ray3D centerRay{{0.0F, 0.0F, -2.0F}, {0.0F, 0.0F, 1.0F}};
        const auto triangleHit = Intersect(triangle, centerRay);
        Check(triangleHit.has_value(), "ray intersects an indexed UI triangle");
        if (triangleHit)
        {
            const float weightSum = triangleHit->barycentric.x +
                triangleHit->barycentric.y + triangleHit->barycentric.z;
            Check(NearlyEqual(weightSum, 1.0F), "triangle barycentric weights");
            Check(NearlyEqual(triangleHit->parameter, 2.0F), "triangle ray parameter");
        }

        DirectX::XMFLOAT4X4 identity{};
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        const PlaneVisual2DSurface plane(
            2.0F, 2.0F, identity);
        const auto planeHit = plane.Raycast(centerRay);
        Check(planeHit.has_value(), "world-space UI plane is raycastable");
        if (planeHit)
        {
            Check(
                NearlyEqual(planeHit->uv.x, 0.5F) &&
                    NearlyEqual(planeHit->uv.y, 0.5F),
                "plane center maps to center UV");
        }
        WorldSpaceVisual2DCanvas worldCanvas(
            {200.0F, 100.0F},
            std::make_unique<PlaneVisual2DSurface>(
                2.0F, 2.0F, identity));
        const auto upperCanvasPoint = worldCanvas.MapPointer(
            Ray3D{{0.0F, 0.5F, -2.0F}, {0.0F, 0.0F, 1.0F}});
        Check(
            upperCanvasPoint.has_value() &&
                NearlyEqual(upperCanvasPoint->x, 0.0F) &&
                NearlyEqual(upperCanvasPoint->y, 25.0F),
            "world surface input maps to centered Y-up Canvas coordinates");

        const RectangleShape rectangle(2.0F, 2.0F);
        const MeshUvVisual2DSurface mesh(rectangle, identity);
        const auto meshHit = mesh.Raycast(centerRay);
        Check(meshHit.has_value(), "mesh UV UI surface is raycastable");
        if (meshHit)
        {
            Check(
                NearlyEqual(meshHit->uv.x, 0.5F) &&
                    NearlyEqual(meshHit->uv.y, 0.5F),
                "barycentric interpolation preserves rectangle UV");
        }

        const CurvedRectangleShape curvedRectangle(
            3.2F,
            2.1F,
            DirectX::XM_PIDIV4,
            32);
        Check(
            curvedRectangle.VertexCount() == 66 &&
                curvedRectangle.IndexCount() == 192,
            "curved rectangle creates a segmented indexed strip");
        const MeshUvVisual2DSurface curvedMesh(
            curvedRectangle, identity);
        const auto curvedHit = curvedMesh.Raycast(centerRay);
        Check(curvedHit.has_value(), "curved UI surface is raycastable");
        if (curvedHit)
        {
            Check(
                NearlyEqual(curvedHit->uv.x, 0.5F) &&
                    NearlyEqual(curvedHit->uv.y, 0.5F),
                "curved surface center maps to center Canvas UV");
        }
        const auto curvedRightHit = curvedMesh.Raycast(
            Ray3D{{1.0F, 0.0F, -2.0F}, {0.0F, 0.0F, 1.0F}});
        Check(
            curvedRightHit.has_value() &&
                curvedRightHit->uv.x > 0.75F &&
                curvedRightHit->uv.x < 0.9F,
            "curved-surface BVH reaches a noncentral triangle group");
        const auto curvedMiss = curvedMesh.Raycast(
            Ray3D{{0.0F, 2.0F, -2.0F}, {0.0F, 0.0F, 1.0F}});
        Check(
            !curvedMiss.has_value(),
            "curved-surface BVH rejects a ray outside its root bounds");
    }

    void TestMeshUvNearestTriangle()
    {
        class LayeredShape final : public Shape
        {
        public:
            LayeredShape()
            {
                // The farther triangle is visited first from the front.
                SetMeshData({
                    {{-1.0F, -1.0F, 1.0F}, {0.0F, 0.0F}},
                    {{0.0F, 1.0F, 1.0F}, {1.0F, 0.0F}},
                    {{1.0F, -1.0F, 1.0F}, {1.0F, 1.0F}},
                    {{-1.0F, -1.0F, 0.0F}, {0.0F, 0.0F}},
                    {{0.0F, 1.0F, 0.0F}, {0.0F, 1.0F}},
                    {{1.0F, -1.0F, 0.0F}, {1.0F, 0.0F}}},
                    {0, 1, 2, 3, 4, 5});
            }
        };

        DirectX::XMFLOAT4X4 identity{};
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        const LayeredShape shape;
        const MeshUvVisual2DSurface surface(shape, identity, true);
        const auto front = surface.Raycast(
            Ray3D{{0.0F, 0.0F, -2.0F}, {0.0F, 0.0F, 1.0F}});
        Check(front.has_value() &&
            NearlyEqual(front->rayParameter, 2.0F) &&
            NearlyEqual(front->worldPosition.z, 0.0F) &&
            NearlyEqual(front->uv.x, 0.25F) && NearlyEqual(front->uv.y, 0.5F),
            "a closer triangle replaces both the ray hit and its interpolated UV");
        const auto back = surface.Raycast(
            Ray3D{{0.0F, 0.0F, 3.0F}, {0.0F, 0.0F, -1.0F}});
        Check(back.has_value() &&
            NearlyEqual(back->rayParameter, 2.0F) &&
            NearlyEqual(back->worldPosition.z, 1.0F) &&
            NearlyEqual(back->uv.x, 0.75F) && NearlyEqual(back->uv.y, 0.25F),
            "a farther triangle cannot replace the nearest hit or its UV");
        Check(!surface.Raycast(
            Ray3D{{0.9F, 0.9F, -2.0F}, {0.0F, 0.0F, 1.0F}}).has_value(),
            "a ray inside the BVH bounds can miss every triangle");
    }

    void TestVisual2DRouting()
    {
        Visual2DCanvas canvas(
            {320.0F, 180.0F},
            CanvasScaleMode::Fixed);
        const auto canvasPoint = [](const float x, const float y)
        {
            return Point{x - 160.0F, 90.0F - y};
        };
        const auto topLeftRect = [](
            const float x,
            const float y,
            const float width,
            const float height)
        {
            return Rect{x, -y - height, width, height};
        };
        auto& button = CreateButton(
            canvas.AnchorNode(Anchor::TopLeft),
            topLeftRect(20.0F, 20.0F, 120.0F, 40.0F),
            L"Apply");

        Visual2DInputRouter router;
        router.Process(
            canvas,
            {canvasPoint(40.0F, 35.0F),
                true, true, true, false, 0.0F, 10});
        Check(button.IsPressed(), "UI button captures a pointer press");
        router.Process(
            canvas,
            {canvasPoint(40.0F, 35.0F),
                true, false, false, true, 0.0F, 20});
        const std::vector<Action> actions = canvas.TakeActions();
        Check(!button.IsPressed(), "UI button releases pointer capture");
        Check(
            actions.size() == 1 &&
                actions[0].type == ActionType::Clicked &&
                actions[0].source == button.Id(),
            "press and release on one button emits a click");

        auto& slider = CreateSlider(
            canvas.AnchorNode(Anchor::TopLeft),
            topLeftRect(20.0F, 80.0F, 200.0F, 40.0F),
            0.0F);
        auto* sliderBehavior = slider.GetComponent<
            SliderBehaviorComponent>();
        router.Process(
            canvas,
            {canvasPoint(30.0F, 100.0F),
                true, true, true, false, 0.0F, 30});
        router.Process(
            canvas,
            {canvasPoint(300.0F, 100.0F),
                true, true, false, false, 0.0F, 40});
        Check(
            sliderBehavior != nullptr &&
                NearlyEqual(sliderBehavior->Value(), 1.0F),
            "captured slider keeps receiving movement outside its bounds");
        router.Process(
            canvas,
            {canvasPoint(300.0F, 100.0F),
                true, false, false, true, 0.0F, 50});

        (void)CreateSprite(
            canvas.AnchorNode(Anchor::TopLeft),
            topLeftRect(240.0F, 20.0F, 40.0F, 40.0F),
            ImageHandle{123});
        const std::vector<DrawPacket> imageCommands =
            canvas.BuildDrawList();
        const bool imageCommandFound = std::any_of(
            imageCommands.begin(),
            imageCommands.end(),
            [](const DrawPacket& command)
            {
                return command.type == DrawPacketType::Image &&
                    command.image.value == 123;
            });
        Check(
            imageCommandFound,
            "UI image widgets emit reusable image draw commands");

        auto& pointerProbe = canvas.CreateNode(
            Anchor::TopLeft,
            "StationaryPointerProbe");
        pointerProbe.SetBounds({240.0F, -130.0F, 60.0F, 30.0F});
        std::size_t hitTestCount{};
        pointerProbe.AddComponent<
            CustomCollider2DComponent>(
            [&hitTestCount](
                const Visual2DNode&,
                const Point localPosition)
            {
                ++hitTestCount;
                const Rect bounds{
                    0.0F, 0.0F, 60.0F, 30.0F};
                return bounds.Contains(localPosition);
            });
        std::size_t moveEventCount{};
        pointerProbe.AddComponent<
            PointerReceiverComponent>(
            [&moveEventCount](
                Visual2DNode&,
                const PointerEvent& event,
                std::vector<Action>&)
            {
                if (event.type == PointerEventType::Move)
                {
                    ++moveEventCount;
                }
            });

        Visual2DInputRouter cachedRouter;
        const PointerInput stationaryPointer{
            canvasPoint(260.0F, 145.0F),
            true, false, false, false, 0.0F, 60};
        cachedRouter.Process(canvas, stationaryPointer);
        cachedRouter.Process(canvas, stationaryPointer);
        Check(
            hitTestCount == 1 && moveEventCount == 1,
            "stationary pointer snapshots skip hit tests and UI moves");
        cachedRouter.InvalidateHitTest();
        cachedRouter.Process(canvas, stationaryPointer);
        Check(
            hitTestCount == 2 && moveEventCount == 2,
            "invalidated UI geometry refreshes a stationary pointer hit");

        auto& combo = CreateComboBox(
            canvas.AnchorNode(Anchor::TopLeft),
            topLeftRect(20.0F, 130.0F, 180.0F, 30.0F),
            {L"Driver 0", L"Driver 1", L"Driver 2", L"Driver 3", L"Driver 4"});
        auto* comboBehavior = combo.GetComponent<
            ComboBoxBehaviorComponent>();
        comboBehavior->SetItemHeight(20.0F);
        comboBehavior->SetMaxVisibleItems(3);

        // A click on the field opens its popup. The popup extends outside the
        // canvas, so this also checks that it is not clipped by the root bounds.
        router.Process(
            canvas,
            {canvasPoint(50.0F, 145.0F),
                true, true, true, false, 0.0F, 60});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 145.0F),
                true, false, false, true, 0.0F, 70});
        Check(
            comboBehavior->IsExpanded(),
            "combo box opens a popup from the field click");

        // Moving the pointer to an unrelated Canvas area must not dismiss the
        // device list. The player may return to the popup and continue input.
        router.Process(
            canvas,
            {canvasPoint(300.0F, 20.0F),
                true, false, false, false, 0.0F, 75});
        Check(
            comboBehavior->IsExpanded(),
            "combo box remains open after losing pointer focus");

        // One normalized wheel tick advances the first visible row by one.
        router.Process(
            canvas,
            {canvasPoint(50.0F, 180.0F),
                true, false, false, false, -1.0F, 80});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 180.0F),
                true, true, true, false, 0.0F, 90});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 180.0F),
                true, false, false, true, 0.0F, 100});
        const std::vector<Action> comboActions =
            canvas.TakeActions();
        Check(
            comboBehavior->SelectedIndex() == 2 &&
                !comboActions.empty() &&
                comboActions.back().type ==
                    ActionType::SelectionChanged,
            "combo box wheel scrolling selects the shifted visible item");

        // Reopen and drag upward by one row before selecting the second row.
        router.Process(
            canvas,
            {canvasPoint(50.0F, 145.0F),
                true, true, true, false, 0.0F, 110});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 145.0F),
                true, false, false, true, 0.0F, 120});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 200.0F),
                true, true, true, false, 0.0F, 130});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 180.0F),
                true, true, false, false, 0.0F, 140});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 180.0F),
                true, false, false, true, 0.0F, 150});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 190.0F),
                true, true, true, false, 0.0F, 160});
        router.Process(
            canvas,
            {canvasPoint(50.0F, 190.0F),
                true, false, false, true, 0.0F, 170});
        Check(
            comboBehavior->SelectedIndex() == 3,
            "combo box drag scrolling selects the shifted visible item");
    }

    void TestVisual2DClipping()
    {
        Visual2DCanvas canvas(
            {200.0F, 100.0F},
            CanvasScaleMode::Fixed);
        auto& viewport = canvas.CreateNode(
            Anchor::Center,
            "Viewport");
        viewport.SetPivot({0.0F, 0.0F});
        viewport.SetBounds({-40.0F, -20.0F, 80.0F, 40.0F});
        viewport.SetClipRect({0.0F, 0.0F, 80.0F, 40.0F});
        auto& child = CreateButton(
            viewport,
            {60.0F, 0.0F, 40.0F, 40.0F},
            L"Clipped");
        child.GetComponent<SpriteVisualComponent>()->
            SetCornerRadius(6.0F);

        const std::vector<DrawPacket> packets =
            canvas.BuildDrawList();
        Check(
            !packets.empty() && std::ranges::all_of(
                packets,
                [](const DrawPacket& packet)
                {
                    return packet.clipBounds.has_value() &&
                        NearlyEqual(packet.clipBounds->x, -40.0F) &&
                        NearlyEqual(packet.clipBounds->y, -20.0F) &&
                        NearlyEqual(packet.clipBounds->width, 80.0F) &&
                        NearlyEqual(packet.clipBounds->height, 40.0F);
                }),
            "Visual2D draw packets inherit a parent-local clip rectangle");
        Check(
            std::ranges::any_of(
                packets,
                [](const DrawPacket& packet)
                {
                    return packet.type ==
                            DrawPacketType::Rectangle &&
                        NearlyEqual(packet.cornerRadius, 6.0F);
                }),
            "Visual2D Sprite packets retain their local corner radius");

        Visual2DInputRouter router;
        router.Process(
            canvas,
            {{50.0F, 0.0F}, true, true, true, false, 0.0F, 10});
        Check(
            router.CapturedNode() != child.Id(),
            "Visual2D clipping rejects pointer hits outside the viewport");
        router.Process(
            canvas,
            {{50.0F, 0.0F}, true, false, false, true, 0.0F, 15});
        router.Process(
            canvas,
            {{30.0F, 0.0F}, true, true, true, false, 0.0F, 20});
        Check(
            router.CapturedNode() == child.Id(),
            "Visual2D clipping preserves pointer hits inside the viewport");
        router.Reset(canvas);

        viewport.ClearClipRect();
        Check(
            !viewport.ClipRect().has_value(),
            "Visual2D clip rectangles can be cleared at runtime");
    }

    void TestVisual2DTreeZOrder()
    {
        Visual2DCanvas canvas(
            {240.0F, 140.0F},
            CanvasScaleMode::Fixed);
        auto& front = CreateButton(
            canvas.Root(),
            {-80.0F, -18.0F, 100.0F, 48.0F},
            L"Front");
        front.SetZIndex(10);
        auto& back = CreateButton(
            canvas.Root(),
            {-100.0F, -50.0F, 180.0F, 100.0F},
            L"Back");

        const std::vector<DrawPacket> commands =
            canvas.BuildDrawList();
        Check(!commands.empty() && commands.back().text && commands.back().text->value == L"Front",
              "larger sibling Z-index paints its complete subtree last");

        Visual2DInputRouter router;
        router.Process(
            canvas,
            {{-60.0F, 10.0F}, true, true, true, false, 0.0F, 10});
        router.Process(
            canvas,
            {{-60.0F, 10.0F}, true, false, false, true, 0.0F, 20});
        std::vector<Action> actions = canvas.TakeActions();
        Check(
            actions.size() == 1 && actions.front().source == front.Id(),
            "hit testing uses the reverse of sibling paint order");

        back.SetZIndex(20);
        const std::vector<DrawPacket> reorderedCommands =
            canvas.BuildDrawList();
        Check(!reorderedCommands.empty() && reorderedCommands.back().text &&
                  reorderedCommands.back().text->value == L"Back",
              "changing a sibling Z-index invalidates the cached paint order");

        router.Process(
            canvas,
            {{-60.0F, 10.0F}, true, true, true, false, 0.0F, 25});
        router.Process(
            canvas,
            {{-60.0F, 10.0F}, true, false, false, true, 0.0F, 26});
        actions = canvas.TakeActions();
        Check(
            actions.size() == 1 && actions.front().source == back.Id(),
            "hit testing observes a dynamically changed cached Z-order");

        // This point is inside only the larger back widget, so its displayed
        // rectangle and interactive region must still agree.
        router.Process(
            canvas,
            {{-90.0F, 40.0F}, true, true, true, false, 0.0F, 30});
        router.Process(
            canvas,
            {{-90.0F, 40.0F}, true, false, false, true, 0.0F, 40});
        actions = canvas.TakeActions();
        Check(
            actions.size() == 1 && actions.front().source == back.Id(),
            "a widget hit box matches its rendered bounds");
    }

    void TestVisual2DAnchorsAndComponents()
    {
        const Visual2DCanvas panelCanvas(
            {320.0F, 210.0F},
            CanvasScaleMode::Fixed);
        const auto panelPoint = MapScreenPointer(
            {40.0F, 50.0F},
            {1280.0F, 720.0F},
            panelCanvas,
            {20.0F, 30.0F});
        Check(
            panelPoint.has_value() &&
                NearlyEqual(panelPoint->x, -140.0F) &&
                NearlyEqual(panelPoint->y, 85.0F),
            "a panel-sized Canvas maps from its independent screen origin");
        const auto capturedOutsidePanel = MapScreenPointer(
            {500.0F, 400.0F},
            {1280.0F, 720.0F},
            panelCanvas,
            {20.0F, 30.0F});
        Check(
            capturedOutsidePanel.has_value() &&
                capturedOutsidePanel->x >
                    panelCanvas.LogicalSize().width * 0.5F &&
                capturedOutsidePanel->y <
                    -panelCanvas.LogicalSize().height * 0.5F,
            "Canvas mapping preserves screen-valid drag positions outside a panel");

        Visual2DCanvas canvas;
        canvas.SetViewportSize({2560.0F, 1080.0F});
        Check(
            NearlyEqual(canvas.PixelScale(), 1.5F) &&
                NearlyEqual(canvas.LogicalSize().height, 720.0F) &&
                NearlyEqual(canvas.LogicalSize().width, 2560.0F / 1.5F),
            "fixed-height Canvas scales from 720 vertical design pixels");
        const auto topLeft = MapScreenPointer(
            {0.0F, 0.0F},
            {2560.0F, 1080.0F},
            canvas);
        const auto bottomRight = MapScreenPointer(
            {2560.0F, 1080.0F},
            {2560.0F, 1080.0F},
            canvas);
        Check(
            topLeft.has_value() && bottomRight.has_value() &&
                NearlyEqual(
                    topLeft->x,
                    -canvas.LogicalSize().width * 0.5F) &&
                NearlyEqual(
                    topLeft->y,
                    canvas.LogicalSize().height * 0.5F) &&
                NearlyEqual(
                    bottomRight->x,
                    canvas.LogicalSize().width * 0.5F) &&
                NearlyEqual(
                    bottomRight->y,
                    -canvas.LogicalSize().height * 0.5F),
            "screen corners map to a centered Canvas with positive Y upward");
        Check(
            !canvas.RemoveNode(
                canvas.AnchorNode(Anchor::TopLeft).Id()),
            "reserved Canvas anchor nodes cannot be deleted");

        auto& centered = canvas.CreateNode(
            Anchor::Center,
            "CenteredSprite");
        centered.SetBounds({0.0F, 0.0F, 100.0F, 60.0F});
        centered.AddComponent<SpriteVisualComponent>();
        centered.AddComponent<RectangleCollider2DComponent>();
        centered.AddComponent<PointerReceiverComponent>();
        const Rect centeredBounds = centered.BoundsInCanvas();
        Check(
            NearlyEqual(
                centeredBounds.x,
                -50.0F) &&
                NearlyEqual(centeredBounds.y, -30.0F),
            "center anchor fixes a node around the screen center");

        auto& right = canvas.CreateNode(
            Anchor::TopRight,
            "RightSprite");
        right.SetBounds({-20.0F, -20.0F, 100.0F, 50.0F});
        right.AddComponent<SpriteVisualComponent>();
        const Rect rightBounds = right.BoundsInCanvas();
        Check(
            NearlyEqual(
                rightBounds.x + rightBounds.width,
                canvas.LogicalSize().width * 0.5F - 20.0F) &&
                NearlyEqual(
                    rightBounds.y + rightBounds.height,
                    canvas.LogicalSize().height * 0.5F - 20.0F),
            "right anchor keeps a fixed inward offset after aspect changes");

        Check(
            centered.RemoveComponent<PointerReceiverComponent>() &&
                centered.GetComponent<
                    PointerReceiverComponent>() == nullptr,
            "Visual2D behavior components can be removed at runtime");

        centered.Transform().SetRotationRollPitchYaw(
            0.0F,
            0.0F,
            DirectX::XM_PIDIV4);
        Visual2DInputRouter router;
        const Point centerPoint{
            0.0F,
            0.0F};
        router.Process(
            canvas,
            {centerPoint, true, true, true, false, 0.0F, 10});
        Check(
            router.CapturedNode() == centered.Id(),
            "rotated Sprite collision uses the same Transform as rendering");
        router.Reset(canvas);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--texture-lifetime")
        return TestTextureLifetime();
    if (argc == 2 && std::string_view(argv[1]) == "--input-state")
        return TestInputState();
    if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--audio-pcm")
        return TestAudioPcm(argc == 3 ? argv[2] : nullptr);
    if (argc == 2 && std::string_view(argv[1]) == "--window-control")
        return TestWindowControl();
    if (argc == 2 && std::string_view(argv[1]) == "--headless-error")
        return TestHeadlessRunError();
    if (argc == 2 && std::string_view(argv[1]) == "--text-rendering")
        return TestTextRendering();
    if (argc == 2 && std::string_view(argv[1]) == "--text-pixels")
        return TestTextPixelOrder();
    if (argc == 2 && std::string_view(argv[1]) == "--image-readback")
        return TestImageReadback();
    if (argc == 2 && std::string_view(argv[1]) == "--scene-manager")
        return TestSceneManager();
    if (argc == 2 && std::string_view(argv[1]) == "--visual2d-pixels")
        return TestVisual2DPixels();
    {

        Visual2DCanvas canvas;
        auto &label = CreateLabel(canvas.Root(), {0, 0, 120, 32}, L"Snapshot");
        auto &text = *label.GetComponent<TextVisualComponent>();
        auto first = canvas.BuildDrawList();
        const auto source = first.back().text;
        text.SetText(L"Snapshot");
        text.SetFont(text.Font());
        text.SetTextColor({1, 0, 0, 1});
        text.SetTextOffset({-20, 0});
        text.SetRepeatOffset(Point{200, 0});
        const auto moved = canvas.BuildDrawList();
        Check(moved.size() == 2 && moved[0].text == source && moved[1].text == source && moved[0].textOffset.x == -20 &&
                  moved[1].textOffset.x == 180,
              "Position, color and a repeated copy preserve immutable text content");
        source->measuredSize = Size{220, 20};
        Check(text.MeasuredSize() && text.MeasuredSize()->width == 220,
              "Backend measurement is available in logical Canvas units");
        text.SetText(L"Changed");
        const auto changed = canvas.BuildDrawList();
        Check(changed[0].text != source && source->value == L"Snapshot" && !text.MeasuredSize(),
              "Content changes publish a new snapshot and reset its measurement");
        text.SetFontWeight(800);
        const auto weighted = canvas.BuildDrawList();
        Check(text.FontWeight() == 800 && weighted[0].text != changed[0].text && weighted[0].text->fontWeight == 800 &&
                  changed[0].text->fontWeight == 400,
              "Font weight changes invalidate shaping without mutating retained content");
        text.SetFontWeight(800);
        Check(canvas.BuildDrawList()[0].text == weighted[0].text, "An unchanged weight preserves content");
        for (const auto invalid : std::array<std::uint16_t, 2>{0, 1000})
        {
            bool rejected = false;
            try
            {
                text.SetFontWeight(invalid);
            }
            catch (const std::invalid_argument &)
            {
                rejected = true;
            }
            Check(rejected && text.FontWeight() == 800, "Invalid weights preserve the previous style");
        }
    }
    {

        struct BatchVisual final : Visual2DComponent
        {
            std::vector<DrawPacket> commands;
            void AppendDrawPackets(std::vector<DrawPacket> &output) const override
            { output.insert(output.end(), commands.begin(), commands.end()); }
        };
        Visual2DCanvas canvas;
        auto &node = canvas.CreateNode(Anchor::TopLeft, "Immutable batch");
        node.SetBounds({20, 30, 200, 100});
        node.SetClipRect(Rect{0, 0, 150, 80});
        auto &visual = node.AddComponent<BatchVisual>();
        auto geometry = std::make_shared<const std::vector<RectanglePrimitive>>(
            std::vector<RectanglePrimitive>{{{1, 2, 3, 4}, {1, 0, 0, .5F}, 2},
                                               {{5, 6, 7, 8}, {0, 1, 0, 1}, 0}});
        DrawPacket batch;
        batch.type = DrawPacketType::RectangleBatch;
        batch.rectangles = geometry;
        DrawPacket text;
        text.type = DrawPacketType::Text;
        text.text =
            std::make_shared<TextContent>(TextContent{L"after geometry", {}, 18, TextAlignment::Leading, {}});
        visual.commands = {batch, text};
        auto retained = canvas.BuildDrawList();
        visual.commands.clear();
        geometry.reset();
        std::vector<DrawPacket> expanded;
        ForEachDrawPrimitive(retained, [&](const auto &primitive) { expanded.push_back(primitive); });
        Check(retained.size() == 2 && DrawPrimitiveCount(retained) == 3 && expanded.size() == 3 &&
                  expanded[0].bounds.x == 1 && expanded[1].color.green == 1 && expanded[2].text &&
                  expanded[2].text->value == L"after geometry" && expanded[0].cornerRadius == 2 &&
                  expanded[0].clipBounds && retained[0].clipBounds &&
                  expanded[0].clipBounds->x == retained[0].clipBounds->x &&
                  expanded[0].nodeTransform._41 == retained[0].nodeTransform._41,
              "Immutable rectangle batches retain lifetime, primitive order, transform and clipping.");
    }
    failureCount += TestInputState();
    TestManagedAudioPlayback();
    TestManagedScreenVisual2D();
    TestCameraMatrices();
    TestPerspectiveFrustum();
    TestOrthographicFrustum();
    TestFrustumAndWorldBoundsUpdates();
    TestPlaneAndLines();
    TestTwoDimensionalQueries();
    TestThreeDimensionalVolumes();
    TestTriangleAndUvSurfaces();
    TestMeshUvNearestTriangle();
    TestVisual2DRouting();
    TestVisual2DClipping();
    TestVisual2DTreeZOrder();
    TestVisual2DAnchorsAndComponents();

    if (failureCount != 0)
    {
        std::cerr << failureCount << " engine test(s) failed.\n";
        return 1;
    }

    std::cout << "All audio, Camera, collision, and Visual2D tests passed.\n";
    return 0;
}
