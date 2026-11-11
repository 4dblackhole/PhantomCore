#pragma once

#include "Shape/Shape.h"

#include <cstdint>

namespace PhantomCore
{
class SphereShape final : public Shape
{
public:
    explicit SphereShape(
        float radius = 1.0F,
        std::uint32_t sliceCount = 32,
        std::uint32_t stackCount = 16);
};

} // namespace PhantomCore
