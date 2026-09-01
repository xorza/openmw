#include "distantland.hpp"

#include <components/misc/constants.hpp>

namespace Rtx
{
    float distantLandReach(float cells, float viewingDistance)
    {
        if (!(cells > 0.0f))
            return viewingDistance;

        return cells * static_cast<float>(Constants::CellSizeInUnits);
    }
}
