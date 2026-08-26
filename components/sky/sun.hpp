#pragma once

#include <osg/Vec3f>
#include <osg/Vec4f>

#include "timeofday.hpp"

namespace Sky
{
    /// Where Morrowind's sun is at an hour, and how much of it there is.
    ///
    /// **The engine keeps five separate dials for one sun** — a light direction, a light colour, a
    /// disc position, a disc colour and a switch saying whether the disc is drawn — and its renderer
    /// never had to make any two of them agree, because a sprite and a lighting matrix do not meet.
    /// This is where those dials are read, and reading them is all it does: `mShare` is what
    /// anything that traces takes, and the rest is what the rasterizer still asks for.
    struct SunPlacement
    {
        /// Where the disc stands, unit. Level with the horizon at sunrise and sunset, and seventy-
        /// nine degrees up at noon.
        ///
        /// **Kept through the night**, where `mShare` is nought and there is no sun: a moon's
        /// crescent points at where the sun would be, so this is still the answer to *where* even
        /// when the answer to *whether* is no.
        osg::Vec3f mPosition;

        /// Where the light travels, unit — so a ray pointing back along it points at the sun. Its
        /// northing and climb are fixed, which is the engine's own simplification and is why this is
        /// not simply `-mPosition`.
        ///
        /// **The rasterizer's, and a tracer takes `-mPosition` instead.** Nothing in a rasterized
        /// frame shows where the two part company; trace it and it shows in the shadows, the glitter
        /// and the haze at once, each around a different sun. Upstream offers the same choice as
        /// `match sunlight to sun`, which this renderer has permanently made.
        osg::Vec3f mDirection;

        /// How much of the sun is over the horizon: nought all night and one through the day,
        /// ramping across dawn and dusk.
        ///
        /// **This is the whole of "is there a sun", and there is no second answer to it.** Every
        /// other way of asking — a night flag, a disc-enabled switch, a disc alpha — was the same
        /// question with its own copy of the answer, and a renderer that shadowed from one while
        /// drawing from another had a sun casting shadows out of an empty sky. What is nought here
        /// lights nothing, casts nothing, glints off nothing and draws nothing.
        float mShare = 0.0f;

        /// Whether the hour falls after dusk, which switches the rasterizer's night skybox.
        ///
        /// **The rasterizer's too**, and it is not `mShare == 0`: the skybox changes at an instant
        /// where the sun fades across an hour, and they disagree at exactly the two boundaries.
        bool mNight = false;
    };

    /// How much of the sun is over the horizon at `hour`.
    ///
    /// Morrowind's own two curves — linear in over the first half of the sunrise window, squared out
    /// across the whole of dusk — with the night that the engine states separately folded in, so the
    /// one number is true at every hour rather than at the ones the caller remembered to check.
    float sunShareAt(float hour, const TimeOfDaySettings& times);

    /// The sun at `hour`.
    ///
    /// **One arithmetic and every caller**: `MWWorld::WeatherManager` runs the game's sky from it and
    /// `openmw-rtxtool` has no weather manager and asks it directly. Every constant in it is
    /// Morrowind's own and the comments say which.
    SunPlacement sunAt(float hour, const TimeOfDaySettings& times);

    /// What the sun's disc is *painted* with, which is not the colour of the light it sends.
    ///
    /// **Morrowind records two sun colours and they are different quantities.** `Sun_*_Color` is
    /// what the world receives — sun and skylight together, which is why its night value is a blue
    /// no sun ever was — and the disc has its own, white for all of the day and warming only as it
    /// goes down. A renderer that paints the disc with the light's colour draws a blue sun through
    /// every dawn and dusk, which is the ramp crossing to night while the disc is still up.
    ///
    /// How much of it there is is `sunShareAt` and is not here: a colour and a quantity are two
    /// things, and bundling them is what let the quantity be forgotten.
    ///
    /// @param sunsetColour `Weather_<name>_Sun_Disc_Sunset_Color`, as the file stores it.
    /// @param ambient the ambient at this same hour, which the clamp below needs.
    osg::Vec3f sunDiscAt(
        float hour, const TimeOfDaySettings& times, const osg::Vec4f& sunsetColour, const osg::Vec4f& ambient);
}
