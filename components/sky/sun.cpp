#include "sun.hpp"

#include <algorithm>
#include <cmath>

namespace Sky
{
    namespace
    {
        /// Morrowind's own sun, hardcoded in the engine it came from: how far east and west it
        /// swings, how far north it sits, and how far down it looks at noon.
        constexpr float sSwing = 400.0f;
        constexpr float sNorthing = 75.0f;
        constexpr float sClimb = 100.0f;
    }

    float sunShareAt(float hour, const TimeOfDaySettings& times)
    {
        // **Nothing at all outside the day, which is the half the engine states elsewhere.** Its own
        // two curves below run on through the night and come back at one, because a rasterizer that
        // has already hidden the disc has no use for the answer; a tracer asks this to decide
        // whether to cast a shadow, and the answer it got was a sun in the middle of the night.
        if (hour <= times.mNightEnd || hour >= times.mNightStart)
            return 0.0f;

        // Squared on the way out, so the sun holds most of itself through dusk and then goes
        // quickly, reaching exactly nought where `sunAt` puts it level with the horizon.
        if (hour >= times.mDayEnd)
        {
            const float fade = std::min(1.0f, (hour - times.mDayEnd) / (times.mNightStart - times.mDayEnd));
            return 1.0f - fade * fade;
        }

        // Linear in over the first half of the sunrise window, and it is the *hour* past dawn rather
        // than a fraction of one — Morrowind's two-hour sunrise arrives at one at the end of it and
        // nothing in the engine bounds it, which mattered nothing while it was only an alpha.
        if (hour <= times.mNightEnd + 0.5f * (times.mDayStart - times.mNightEnd))
            return std::min(1.0f, hour - times.mNightEnd);

        return 1.0f;
    }

    SunPlacement sunAt(float hour, const TimeOfDaySettings& times)
    {
        const float sunrise = times.mNightEnd;
        const float nightStart = times.mNightStart;

        // **Both times shifted into a twenty-four hour window beginning at sunrise**, which is what
        // makes the two halves meet: an hour before dawn is late in the *previous* night rather than
        // early in a day it has not reached (`apps/openmw/mwworld/weather.cpp:583`).
        float adjustedHour = hour;
        float adjustedNightStart = nightStart;
        if (hour < sunrise)
            adjustedHour += 24.0f;
        if (nightStart < sunrise)
            adjustedNightStart += 24.0f;

        const bool night = adjustedHour >= adjustedNightStart;
        const float dayDuration = adjustedNightStart - sunrise;
        const float nightDuration = 24.0f - dayDuration;

        // One at the eastern end, minus one at the western, and back again over the night — so the
        // day's speed and the night's may differ and the two still meet at exactly minus one.
        const float orbit = night ? 2.0f * ((adjustedHour - adjustedNightStart) / nightDuration) - 1.0f
                                  : 1.0f - 2.0f * ((adjustedHour - sunrise) / dayDuration);

        osg::Vec3f direction(-sSwing * orbit, sNorthing, -sClimb);

        // **The disc's height is not the light's, and this line is the whole difference**
        // (`apps/openmw/mwrender/renderingmanager.cpp:577`). The east-west swing is shared, but the
        // disc climbs `swing - |east|` — nought at either end of the day and the full swing at noon
        // — so it comes up out of the horizon and goes back down into it, while the light stays at
        // its fixed angle overhead. Nothing in the rasterizer could tell the two apart, because it
        // hides the disc at night and reads only the light after that.
        osg::Vec3f position = -direction;
        position.z() = sSwing - std::abs(position.x());

        direction.normalize();
        position.normalize();

        return SunPlacement{
            .mPosition = position, .mDirection = direction, .mShare = sunShareAt(hour, times), .mNight = night
        };
    }

    osg::Vec3f sunDiscAt(
        float hour, const TimeOfDaySettings& times, const osg::Vec4f& sunsetColour, const osg::Vec4f& ambient)
    {
        const float preSunset = times.getSetting("Sun").mPreSunsetTime;
        if (hour < times.mDayEnd - preSunset)
            return osg::Vec3f(1.0f, 1.0f, 1.0f);

        const float factor = preSunset > 0.0f ? std::min(1.0f, (hour - (times.mDayEnd - preSunset)) / preSunset) : 1.0f;

        osg::Vec4f colour = osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f) * (1.0f - factor) + sunsetColour * factor;

        // **Morrowind's own mistake, replicated because the recorded colour is not what it
        // produced.** The original applied the disc's colour to the ambient term as well, and the
        // fixed pipeline then clamped the sum to one — so a channel that clipped and one that did
        // not came out of the sunset a different hue from the file's. Reading
        // `Sun_Disc_Sunset_Color` straight gives a sun nobody has ever seen in this game.
        colour += osg::componentMultiply(colour, ambient);

        return osg::Vec3f(std::min(1.0f, colour.x()), std::min(1.0f, colour.y()), std::min(1.0f, colour.z()));
    }
}
