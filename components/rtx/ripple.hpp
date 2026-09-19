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

        /// The ring's radius, in world units. No default, because the game states the size a
        /// footfall presses (`MWRender::RippleEmitters`) and this core must not restate it.
        float mSize = 0.0f;
    };
}
