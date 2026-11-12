#include "Audio/AudioPcmTests.h"
#include "App/HeadlessRunTests.h"
#include "App/ImageReadbackTests.h"
#include "App/SceneManagerTests.h"
#include "App/Visual2DPixelTests.h"
#include "App/TextPixelTests.h"
#include "App/TextRenderingTests.h"
#include "App/WindowControlTests.h"
#include "App/InputStateTests.h"
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
    if (argc == 2 && std::string_view(argv[1]) == "--window-control")
        return TestWindowControl();
    if (argc == 2 && std::string_view(argv[1]) == "--draw-statistics")
        return TestDrawStatistics();
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
    const int cpuResult = TestCollisionAndVisual2D();
    const int audioResult = TestManagedPlayback();
    return cpuResult != 0 || audioResult != 0 ? 1 : 0;
}
