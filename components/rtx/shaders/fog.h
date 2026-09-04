#ifndef OPENMW_COMPONENTS_RTX_SHADERS_FOG_H
#define OPENMW_COMPONENTS_RTX_SHADERS_FOG_H

#include "portable.h"

#ifdef RTX_HOST
#include <algorithm>
#include <cmath>

namespace Rtx::Shaders
{
    using std::abs;
    using std::exp;
    using std::max;
    using std::min;
#endif

    /// The mean of exp(-t) over [0, depth], including its limit at zero.
    RTX_SHADER float fogExponentialMean(float depth)
    {
        // The next Taylor term at 0.01 is below 1e-10; subtraction loses precision near zero.
        if (depth < 0.01f)
            return 1.0f - depth * (0.5f - depth * (1.0f / 6.0f - depth / 24.0f));

        return (1.0f - exp(-depth)) / depth;
    }

    /// Optical depth through a capped exponential layer, with heights relative to its base.
    RTX_SHADER float fogLayerDepth(float extinction, float scale, float from, float to, float distance, bool water)
    {
        if (from >= 0.0f && to >= 0.0f)
            return extinction * distance * exp(-min(from, to) / scale) * fogExponentialMean(abs(to - from) / scale);

        if (from <= 0.0f && to <= 0.0f)
            return water ? 0.0f : extinction * distance;

        const float above = max(from, to);
        const float fraction = above / abs(to - from);
        return extinction * distance
            * (fraction * fogExponentialMean(above / scale) + (water ? 0.0f : 1.0f - fraction));
    }

    /// Optical depth from a point to the sky along an upward directional light.
    RTX_SHADER float fogLightDepth(float extinction, float scale, float height, float climb)
    {
        return extinction * (scale * exp(-max(height, 0.0f) / scale) + max(-height, 0.0f)) / climb;
    }

    /// Single scattering with both the view and light paths attenuated by the same layer.
    RTX_SHADER float fogLightIntegral(float depth, float lightFrom, float lightTo, float climbRatio)
    {
        // In optical-depth coordinates the exponent is lightFrom + (1 - climbRatio) * t.
        // Factoring out its smaller endpoint avoids overflow when the view climbs toward the light.
        return exp(-min(lightFrom, depth + lightTo)) * depth * fogExponentialMean(abs(1.0f - climbRatio) * depth);
    }

#ifdef RTX_HOST
}
#endif

#endif
