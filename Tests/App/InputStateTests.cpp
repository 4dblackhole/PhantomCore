#include "InputStateTests.h"
#include "PhantomCore.h"
#include <iostream>

// Test-only access keeps event injection outside the consumer API.
using namespace PhantomCore;

namespace PhantomCore
{
struct InputStateTestAccess final
{
    static void Begin(InputState &input)
    {
        input.BeginUpdate();
    }
    static void Reset(InputState &input)
    {
        input.Reset();
    }
    static void Move(InputState &input, std::int64_t time)
    {
        input.AddMouseDelta(1, -1, time);
    }
    static void Wheel(InputState &input, std::int64_t time)
    {
        input.AddMouseWheel(1, time);
    }
    static void Key(InputState &input, bool down, std::int64_t time)
    {
        input.SetKey('F', down, time);
    }
};
} // namespace PhantomCore


int TestInputState()
{

    InputState input;
    for (int turn = 0; turn < 2; ++turn)
    {
        InputStateTestAccess::Begin(input);
        for (std::int64_t i = 0; i < 1024; ++i)
        {
            InputStateTestAccess::Move(input, 4 * i);
            InputStateTestAccess::Key(input, true, 4 * i + 1);
            InputStateTestAccess::Wheel(input, 4 * i + 2);
            InputStateTestAccess::Key(input, false, 4 * i + 3);
        }
        const auto events = input.Events();
        if (events.size() != 4096 || input.IsKeyDown('F') || !input.WasKeyPressed('F') || !input.WasKeyReleased('F'))
            return 1;
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            constexpr std::array types{InputEventType::MouseMoved, InputEventType::KeyPressed,
                                       InputEventType::MouseWheel, InputEventType::KeyReleased};
            if (events[i].type != types[i % 4] || events[i].performanceCounterTicks != static_cast<std::int64_t>(i))
                return 1;
        }
    }
    InputStateTestAccess::Key(input, true, 4096);
    InputStateTestAccess::Begin(input);
    if (!input.Events().empty() || !input.IsKeyDown('F') || input.WasKeyPressed('F') || input.WasKeyReleased('F'))
        return 1;
    InputStateTestAccess::Key(input, false, 4097);
    if (input.Events().size() != 1 || input.IsKeyDown('F') || !input.WasKeyReleased('F'))
        return 1;
    InputStateTestAccess::Reset(input);
    if (!input.Events().empty() || input.WasKeyReleased('F'))
        return 1;
    std::cout << "Timestamped input: 4096 mixed events, ordered key edges, "
                 "snapshot and capacity reuse passed.\n";
    return 0;
}
