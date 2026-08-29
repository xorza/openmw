#include "fogbuilder.hpp"

#include <algorithm>
#include <cmath>

#include "distantland.hpp"
#include "lightbuilder.hpp"

namespace Rtx
{
    float fogExtinction(float depth, float over)
    {
        // The original engine reads a depth of zero as no fog at all rather than as a ramp starting
        // at the view distance, and so does this.
        if (!(depth > 0.0f))
            return 0.0f;

        return std::log(2.0f) / (over * (1.0f - 0.5f * depth));
    }

    float fogLift(float depth, float wind)
    {
        return depth / sClearFogDepth * (1.0f + wind * sFogWindLift);
    }

    osg::Vec3f fogColour(const osg::Vec3f& skyMean, const osg::Vec3f& hue)
    {
        const float brightest = std::max({ hue.x(), hue.y(), hue.z(), 1e-4f });
        return osg::componentMultiply(skyMean, hue / brightest);
    }

    Fog exteriorFog(const osg::Vec3f& colour, float depth, float wind)
    {
        const float reach = distantLandReach();

        return Fog{
            .mColour = colour,
            .mExtinction = fogExtinction(depth, reach),
            .mUniform = 0.0f,

            // **The same record read a second time, and deliberately.** `fogExtinction` reads it as
            // the view-range ramp the original engine wrote it as, and takes a half-life off it;
            // this reads it as what the field is called — a *depth* — and takes a layer height. A
            // weather with more fog has fog that reaches higher, so the two readings agree about
            // which way each weather moves and disagree only about the curve.
            .mLift = fogLift(depth, wind),
            .mWind = wind,
            .mEdge = reach,
        };
    }

    Fog roomFog(const osg::Vec3f& colour, float depth)
    {
        return Fog{
            .mColour = colour,

            // A room is measured against one fixed distance and not against how much world is
            // built outside it, nor against how much the player asked to see: a cellar does not
            // clear because the sky got bigger. `sInteriorFogReach` is where that is said.
            .mExtinction = fogExtinction(depth, sInteriorFogReach),

            // A room is smaller than one bank of fog, and its air is still.
            .mUniform = 1.0f,

            // A room has no weather over it to stand its air up or carry it anywhere, and the
            // layer it holds is the one `FOG_HEIGHT` names.
            .mLift = 1.0f,
            .mWind = 0.0f,

            .mEdge = 0.0f,
        };
    }
}
