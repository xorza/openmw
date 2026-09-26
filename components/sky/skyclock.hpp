#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Sky
{
    /// The `timescale` global as Morrowind ships it: thirty game seconds to each real one.
    constexpr float sVanillaTimeScale = 30.0f;

    /// How far the sky's own clock moves over `seconds` of simulation at `timeScale` game seconds to
    /// each: the one clock the deck's scroll and the fog's drift run on, beside the hour the sun and
    /// the stars already read. The weather manager's own crossing and thunder stay on the frame's
    /// clock, as upstream paces them: the game's pace is not this renderer's to change.
    ///
    /// **Real seconds at Morrowind's `timescale`, and a time-lapse at any other.** Morrowind paces
    /// its deck and its wind in real time and its sun in game time, so a sped-up clock raced the
    /// sun under a sky that stood still. Divided by the shipped scale rather than stated in game
    /// seconds, so `Cloud_Speed` and `FOG_GALE` keep the meaning they were authored with, and a run
    /// at the shipped `timescale` is the run it always was — the ratio first, so that at thirty the
    /// step is the simulation's own to the bit. A negative `timescale` holds the sky rather than
    /// running it backwards, as the engine's own hour stops at midnight under one.
    inline float skyStep(const float seconds, const float timeScale)
    {
        return seconds * (std::max(timeScale, 0.0f) / sVanillaTimeScale);
    }

    /// How far the star sphere has turned at `gameSeconds` of game time, in radians about the
    /// vertical, in [-π, π]: once round in four days, counter-clockwise as the player sees it, as
    /// Morrowind turns it. **Both renderers turn their stars by this one function**, the
    /// rasterizer's `SkyManager::update` and the ray tracer's `SkyReader`: an upstream change to
    /// the rule conflicts at the rasterizer's call, and is made here for both. Taken round the
    /// circle in double and only then narrowed, because a float's angle a hundred days in resolves
    /// a hundredth of a degree.
    inline float starRoll(const double gameSeconds)
    {
        constexpr double fourDays = 3600.0 * 96.0;
        return static_cast<float>(
            std::remainder(gameSeconds * (-2.0 * std::numbers::pi) / fourDays, 2.0 * std::numbers::pi));
    }

    /// The clocks a renderer that draws no dome runs its sky on: how far the cloud deck has scrolled,
    /// in texture units, and how many of the sky's own seconds have passed. Neither is a function
    /// of the hour: the deck runs on the weather's speed. The rasterizer's `SkyManager` keeps its
    /// own deck at upstream's pace, and neither renderer reads the other's.
    struct SkyClock
    {
        /// A double, because the fog's drift is the difference of two readings, and ten hours in a
        /// float resolves 0.0039 s — a quarter of a frame at sixty.
        double mSeconds = 0.0;
        float mCloudScroll = 0.0f;

        /// Moves both clocks on by one frame of `seconds` at `timeScale`, under a deck driven at
        /// `cloudSpeed`: by the sky's clock, because `Cloud_Speed` is a rate over real seconds.
        void step(const float seconds, const float timeScale, const float cloudSpeed)
        {
            const float skySeconds = skyStep(seconds, timeScale);
            mSeconds += static_cast<double>(skySeconds);

            // The deck's sheet repeats every four texture units, which is where a scroll wraps.
            mCloudScroll += skySeconds * cloudSpeed / 400.f;
            if (mCloudScroll >= 4.f)
                mCloudScroll -= 4.f;
        }
    };
}
