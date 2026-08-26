#include "fogbuilder.hpp"

#include <cmath>

#include <components/esm3/loadcell.hpp>
#include <components/settings/values.hpp>

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

    float interiorFogReach(float measured)
    {
        return measured * 10.0f;
    }

    Fog interiorFog(const ESM::Cell& cell)
    {
        if (!cell.mHasAmbi)
            return {};

        return Fog{
            .mColour = decodeColour(cell.mAmbi.mFog),

            // A room is measured against the view range the original engine measured it against, and
            // not against how much world is built outside it: a cellar does not clear because the
            // sky got bigger. Stretched from there, because a medium is not the ramp that range was
            // written for — `interiorFogReach` is where that is said.
            .mExtinction
            = fogExtinction(cell.mAmbi.mFogDensity, interiorFogReach(Settings::camera().mViewingDistance)),

            // A room is smaller than one bank of fog, and its air is still.
            .mUniform = 1.0f,
        };
    }
}
