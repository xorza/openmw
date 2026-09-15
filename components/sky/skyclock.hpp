#pragma once

#include <algorithm>

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
}
