#pragma once

#include <osg/Vec3f>

#include "shaders/visibility.h"

namespace Rtx
{
    /// The sun glare fader, as the game states it — `glare.h`. A wash over the picture after the
    /// curve, so the display chain's and no trace's: it rides beside the frame block, not in it.
    struct SunGlare
    {
        /// `Weather_Sun_Glare_Fader_Color` doubled and clamped, in the display's own values and not
        /// in light, because that is the space the rasterizer adds it in.
        osg::Vec3f mColour;

        /// `_Angle_Max` in radians, past which the wash is nothing.
        float mAngleMax = 0.0f;

        /// `_Max` times the time-of-day fade times the weather's `Glare_View`, and nought where no
        /// sun is drawn.
        float mStrength = 0.0f;

        /// How much of `mColour` `frame` lays over the picture before the share of the sun the eye
        /// could see is multiplied in: the strength, faded by how far the eye's axis stands from the
        /// sun — `SunGlareCallback`'s `1 - min(1, angle / angleMax)`. Nought for no fader, and
        /// nought past the angle.
        float amountFor(const Shaders::VisibilityConstants& frame) const;
    };
}
