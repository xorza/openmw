#include "sun.hpp"

#include <algorithm>
#include <cmath>

namespace Sky
{
    namespace
    {
        /// Morrowind's own sun, hardcoded in the engine it came from and in
        /// `MWWorld::WeatherManager::update`: how far east and west it swings, and how far north it
        /// sits.
        constexpr float sSwing = 400.0f;
        constexpr float sNorthing = 75.0f;

        /// How long the day is, in hours.
        ///
        /// **A night that begins before the sunrise it followed belongs to the next day**, which is
        /// the wrap every hour of this file is read through: an hour before dawn is late in the
        /// previous night rather than early in a day it has not reached.
        float dayLength(const TimeOfDaySettings& times)
        {
            const float nightStart
                = times.mNightStart < times.mNightEnd ? times.mNightStart + 24.0f : times.mNightStart;

            return nightStart - times.mNightEnd;
        }
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
        // quickly, reaching exactly nought where the weather manager puts it level with the horizon.
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

    float sunDescentPerHour(const TimeOfDaySettings& times)
    {
        const float day = dayLength(times);
        if (!(day > 0.0f))
            return 0.0f;

        // The disc stands at `sSwing - |east|` over a horizontal `hypot(sSwing, sNorthing)`, so near
        // either end its elevation is that ratio times what the orbit has left to run — and the
        // orbit crosses two units over the whole day.
        return 2.0f * sSwing / std::hypot(sSwing, sNorthing) / day;
    }
}
