#include "App/RenderScheduleTests.h"
#include "Audio/AudioMixerClockTests.h"
#include "Collision/CollisionTests.h"

int main()
{
    const int cpuResult = TestCollisionAndVisual2D();
    const int audioClockResult = TestAudioMixerClockEstimator();
    const int renderScheduleResult = TestRenderSchedule();
    return cpuResult != 0 || audioClockResult != 0 || renderScheduleResult != 0 ? 1 : 0;
}
