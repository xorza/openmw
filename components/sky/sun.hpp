#pragma once

#include "timeofday.hpp"

namespace Sky
{
    /// How much of the sun is over the horizon at `hour`.
    ///
    /// Morrowind's own two curves — linear in over the first half of the sunrise window, squared out
    /// across the whole of dusk — with the night that the engine states separately folded in, so the
    /// one number is true at every hour rather than at the ones the caller remembered to check.
    ///
    /// **One arithmetic and two readers**: `MWWorld::WeatherManager` writes it as the disc's alpha,
    /// and the ray tracer reads it back off the hour for the deck that keeps the sun after the
    /// ground has lost it (`Rtx::sunShareAloft`).
    float sunShareAt(float hour, const TimeOfDaySettings& times);

    /// How fast the disc's elevation changes near either end of the day, in radians per hour.
    ///
    /// **What a layer standing above the ground has to convert its own horizon into.** Morrowind's
    /// sunset is a clock and not a horizon — `sunShareAt` ramps on the hour and the weather manager
    /// puts the disc level at exactly `mNightStart` — so nothing anywhere takes an elevation, and
    /// something that keeps the sun a fraction of a degree longer has to say how long that is in
    /// hours instead.
    ///
    /// Constant, because the disc's height is `400 - |east|` and the east-west swing is linear in
    /// the hour: the elevation runs straight into the horizon rather than curving into it, which is
    /// the one place Morrowind's arc is kinder than a real one. Eight degrees an hour over the
    /// shipped fourteen-hour day.
    float sunDescentPerHour(const TimeOfDaySettings& times);
}
