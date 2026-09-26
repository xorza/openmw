#include "sunglare.hpp"

#include <algorithm>
#include <cmath>

namespace Rtx
{
    float SunGlare::amountFor(const Shaders::VisibilityConstants& frame) const
    {
        if (mStrength <= 0.0f || mAngleMax <= 0.0f)
            return 0.0f;

        // `getAngleToSunInRadians`: the eye's own forward against the sun's, both unit. Clamped
        // before the arc cosine, which a dot a rounding past one would hand a NaN.
        const osg::Vec3f forward = frame.mCamera.mForward;
        const osg::Vec3f sun = frame.mSun.mDirection;
        const float cosine
            = std::clamp((forward * sun) / std::max(forward.length() * sun.length(), 1.0e-6f), -1.0f, 1.0f);
        const float angle = std::acos(cosine);

        return mStrength * (1.0f - std::min(1.0f, angle / mAngleMax));
    }
}
