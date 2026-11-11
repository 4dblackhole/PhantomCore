#pragma once

#include "Shape/Shape.h"

// Cube centered at the origin. Vertices are duplicated per face so each
// face has an unambiguous normal and complete UV range.
namespace PhantomCore
{
class CubeShape final : public Shape
{
public:
    explicit CubeShape(float sideLength = 2.0F);
};

} // namespace PhantomCore
