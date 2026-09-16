#pragma once

#include <osg/Vec2f>

namespace Rtx
{
    /// One thing that disturbed the water this frame, in world units on the water's plane: where,
    /// and how wide a ring it presses. What `apps/openmw/mwrender/rtx/rippleemitters.cpp` decides
    /// for a wading actor or a strike, and what `RipplePass` presses into its field.
    struct RippleImpulse
    {
        osg::Vec2f mAt;

        /// The ring's radius, in world units: twelve for a footfall, as `RippleSimulation::emitRipple`
        /// states it.
        float mSize = 12.0f;
    };
}
