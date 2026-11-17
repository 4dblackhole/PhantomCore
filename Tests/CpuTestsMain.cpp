#include "Audio/AudioMixerClockTests.h"
#include "Collision/CollisionTests.h"

int main()
{
    const int cpuResult = TestCollisionAndVisual2D();
    const int audioClockResult = TestAudioMixerClockEstimator();
    return cpuResult != 0 || audioClockResult != 0 ? 1 : 0;
}
