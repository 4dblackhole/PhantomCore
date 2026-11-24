#include "Audio/AudioClockSnapshotTests.h"
#include "Audio/AudioMixerClockTests.h"
#include "Audio/AudioPcmTests.h"
#include "App/HeadlessRunTests.h"
#include "App/DeviceLossTests.h"
#include "App/CrashDumpTests.h"
#include "App/MainLoopWatchTests.h"
#include "App/ImageReadbackTests.h"
#include "App/SceneManagerTests.h"
#include "App/Visual2DPixelTests.h"
#include "App/PixelImageTests.h"
#include "App/FileImageTests.h"
#include "App/TextPixelTests.h"
#include "App/TextRenderingTests.h"
#include "App/WindowControlTests.h"
#include "App/InputStateTests.h"
#include "App/RenderScheduleTests.h"
#include "App/TextureLifetimeTests.h"
#include "App/TextureAssetTests.h"
#include "Collision/CollisionTests.h"
#include "Audio/ManagedPlaybackTests.h"
#include <string_view>

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--texture-assets")
        return TestTextureAssets(argv[2]);
    if (argc == 2 && std::string_view(argv[1]) == "--texture-lifetime")
        return TestTextureLifetime();
    if (argc == 2 && std::string_view(argv[1]) == "--input-state")
        return TestInputState();
    if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--audio-pcm")
        return TestAudioPcm(argc == 3 ? argv[2] : nullptr);
    if (argc == 2 && std::string_view(argv[1]) == "--audio-clock")
        return TestAudioClockSnapshot();
    if (argc == 2 && std::string_view(argv[1]) == "--window-control")
        return TestWindowControl();
    if (argc == 2 && std::string_view(argv[1]) == "--draw-statistics")
        return TestDrawStatistics();
    if (argc == 2 && std::string_view(argv[1]) == "--render-rate")
        return TestRenderRateModes();
    if (argc == 2 && std::string_view(argv[1]) == "--headless-error")
        return TestHeadlessRunError();
    if (argc == 2 && std::string_view(argv[1]) == "--device-loss")
        return TestDeviceLoss();
    if (argc == 2 && std::string_view(argv[1]) == "--main-loop-watch")
        return TestMainLoopWatch();
    if (argc == 2 && std::string_view(argv[1]) == "--crash-dump")
        return TestCrashDump();
    if (argc == 4 && std::string_view(argv[1]) == "--crash-dump-child")
        return RunCrashDumpChild(argv[2], argv[3]);
    if (argc == 2 && std::string_view(argv[1]) == "--text-rendering")
        return TestTextRendering();
    if (argc == 2 && std::string_view(argv[1]) == "--text-pixels")
        return TestTextPixelOrder();
    if (argc == 2 && std::string_view(argv[1]) == "--image-readback")
        return TestImageReadback();
    if (argc == 2 && std::string_view(argv[1]) == "--scene-manager")
        return TestSceneManager();
    if (argc == 2 && std::string_view(argv[1]) == "--pixel-images")
        return TestPixelImages();
    if (argc == 2 && std::string_view(argv[1]) == "--file-images")
        return TestFileImages();
    if (argc == 2 && std::string_view(argv[1]) == "--visual2d-pixels")
        return TestVisual2DPixels();
    const int cpuResult = TestCollisionAndVisual2D();
    const int audioResult = TestManagedPlayback();
    const int audioClockResult = TestAudioMixerClockEstimator();
    const int renderScheduleResult = TestRenderSchedule();
    const bool failed = cpuResult != 0 || audioResult != 0;
    return failed || audioClockResult != 0 || renderScheduleResult != 0 ? 1 : 0;
}
