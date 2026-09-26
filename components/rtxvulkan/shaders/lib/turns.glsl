#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TURNS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TURNS_GLSL

/// How far into its turn something that turns `rate` times a second is, `seconds` after nought: the
/// fraction of `rate * seconds`, from nought up to one.
///
/// **Exact to a float's last place however long the clock has run.** The clock arrives as two floats
/// whose sum is the host's double, `Rtx::splitSeconds`. The product of the larger half is its rounded
/// value plus an error `fma` recovers exactly, and the fraction of a float is exact, so the only
/// rounding left is on numbers under one. A phase taken as `rate * seconds` in one float drifts by the
/// clock's own rounding times the rate: a quarter of a radian of a wave after a hundred hours.
float turnsAt(float rate, vec2 seconds)
{
    precise float whole = rate * seconds.x;
    const float lost = fma(rate, seconds.x, -whole);
    return fract(fract(whole) + lost + rate * seconds.y);
}

#endif
