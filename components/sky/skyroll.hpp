#pragma once

namespace Sky
{
    /// How far the sky's two moving parts have turned.
    ///
    /// **The clouds and the stars move on clocks of their own**, neither of which is the hour of the
    /// day: a deck scrolls at the speed its weather records and the stars come round once every four
    /// days whatever the weather is doing. Morrowind's renderer kept both as loose floats inside its
    /// sky manager, which is where they still would be if only one renderer needed them.
    struct SkyRoll
    {
        /// The deck's scroll along the cloud texture's `v`, wrapped into `[0, 4)`.
        ///
        /// Four rather than one because that is the range the engine's own texture matrix runs over,
        /// and the wrap has to happen somewhere before a float that only ever grows stops being able
        /// to resolve a frame's worth of movement.
        float mClouds = 0.0f;

        /// The star field's roll about the zenith, in radians. Morrowind turns it once every four
        /// days, which is the same three-day-ish clock the moons keep and is not the sun's.
        float mStars = 0.0f;

        /// Advances both by `seconds` of wall clock.
        ///
        /// @param cloudSpeed the weather's `Cloud_Speed`, which is the only thing that differs
        ///        between a still overcast and a scudding storm.
        /// @param timeScale how many game seconds a real one is worth, which the stars always follow
        ///        and the clouds follow only where the content asks them to.
        /// @param timescaleClouds `Weather_Timescale_Clouds`: whether the deck moves on the world's
        ///        clock or on the player's. Off, a cloud crosses the sky at the same rate whether an
        ///        hour of game time takes a minute or an afternoon.
        void advance(float seconds, float cloudSpeed, float timeScale, bool timescaleClouds);
    };
}
