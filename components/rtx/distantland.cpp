#include "distantland.hpp"

#include <components/misc/constants.hpp>
#include <components/settings/values.hpp>

namespace Rtx
{
    float distantLandReach()
    {
        const float cells = Settings::rtx().mDistantLandCells;

        // Zero hands the decision back to the rasterizer's knob, which is the escape hatch rather
        // than the default: it is what everything not looking for distance gets.
        if (!(cells > 0.0f))
            return Settings::camera().mViewingDistance;

        return cells * static_cast<float>(Constants::CellSizeInUnits);
    }
}
