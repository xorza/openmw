#pragma once

#include <cmath>

#include "../mwworld/ptr.hpp"

namespace MWBase
{
    class World;
}

namespace MWRender
{
    /// Whether `actor` presses a ring into the water it stands in: in it and not under it, or
    /// walking on it. The game's rule, which the rasterizer's ripples and the ray tracer's both ask.
    bool isWading(const MWBase::World& world, const MWWorld::ConstPtr& actor);

    /// Whether something that struck at `height` rings water whose surface is at `waterHeight`:
    /// within twenty units of it, above or below.
    inline bool strikesWater(float height, float waterHeight)
    {
        return std::abs(height - waterHeight) < 20.0f;
    }

    /// The ring a footfall or a strike presses, in world units.
    inline constexpr float sRippleSize = 12.0f;
}
