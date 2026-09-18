#ifndef OPENMW_COMPONENTS_SKY_SKYCLOCK_H
#define OPENMW_COMPONENTS_SKY_SKYCLOCK_H

#include <algorithm>

#include <osg/Math>

namespace Sky
{
    /// The `timescale` global as Morrowind ships it: thirty game seconds to each real one.
    constexpr float sVanillaTimeScale = 30.0f;

    /// How far the sky's own clock moves over `seconds` of simulation at `timeScale` game seconds to
    /// each: the one clock a weather's crossing, its thunder, the deck's scroll and the fog's drift
    /// run on, beside the hour the sun and the stars already read.
    ///
    /// **Real seconds at Morrowind's `timescale`, and a time-lapse at any other.** Morrowind paces
    /// a crossing, its deck and its wind in real time and its sun in game time, so a sped-up clock
    /// raced the sun under a sky that stood still. Divided by the shipped scale rather than stated
    /// in game seconds, so `Transition_Delta`, `Cloud_Speed` and `FOG_GALE` keep the meaning they
    /// were authored with, and a run at the shipped `timescale` is the run it always was — the
    /// ratio first, so that at thirty the step is the simulation's own to the bit. A negative
    /// `timescale` holds the sky rather than running it backwards: a crossing cannot un-cross, and
    /// the engine's own hour stops at midnight under one.
    inline float skyStep(const float seconds, const float timeScale)
    {
        return seconds * (std::max(timeScale, 0.0f) / sVanillaTimeScale);
    }

    /// The clocks a renderer that draws no dome runs its sky on: how far the cloud deck has scrolled,
    /// in texture units, how far the star sphere has rolled, in radians, and how many of the sky's
    /// own seconds have passed. Neither is a function of the hour: the deck runs on the weather's
    /// speed and the stars come round once in four days. The rasterizer's `SkyManager` keeps the
    /// same two inside itself, at upstream's pace, and neither renderer reads the other's.
    struct SkyClock
    {
        /// A double, because the fog's drift is the difference of two readings, and ten hours in a
        /// float resolves 0.0039 s — a quarter of a frame at sixty.
        double mSeconds = 0.0;
        float mCloudScroll = 0.0f;
        float mStarRoll = 0.0f;

        /// Moves every clock on by one frame of `seconds` at `timeScale`, under a deck driven at
        /// `cloudSpeed`. The deck by the sky's clock, because `Cloud_Speed` is a rate over real
        /// seconds; the stars by game time, as the rasterizer rolls them.
        void step(const float seconds, const float timeScale, const float cloudSpeed)
        {
            const float skySeconds = skyStep(seconds, timeScale);
            mSeconds += static_cast<double>(skySeconds);

            // The deck's sheet repeats every four texture units, which is where a scroll wraps.
            mCloudScroll += skySeconds * cloudSpeed / 400.f;
            if (mCloudScroll >= 4.f)
                mCloudScroll -= 4.f;

            // rotate the stars by 360 degrees every 4 days
            mStarRoll += timeScale * seconds * osg::DegreesToRadians(360.f) / (3600 * 96.f);
        }
    };
}

#endif
