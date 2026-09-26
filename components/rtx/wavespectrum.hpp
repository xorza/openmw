#pragma once

#include "shaders/scene.h"

namespace Rtx
{
    /// The shortest wave the spectrum carries, in world units — a band limit in time as much as in
    /// space, because the shortest waves decide the caustics and how fast they reshuffle. Carried
    /// down to eighteen units the seabed read as stripes tearing rather than as water.
    inline constexpr float sShortestWave = 32.0f;

    /// What the sea is doing, in the numbers a spectrum needs: TMA — JONSWAP under Kitaigorodskii's
    /// shallow-water attenuation, the coastal-shelf correction this water needs — spread over
    /// directions by Donelan-Banner, so a sum of plane waves does not draw a lattice.
    struct SeaState
    {
        /// The average height of the highest third of the waves, in world units.
        float mSignificantHeight = 9.4f;

        /// The wavelength carrying the most energy, in world units.
        float mPeakWavelength = 420.0f;

        /// Depth of the shelf the spectrum is attenuated against.
        float mDepth = 300.0f;

        /// Two equal states are one sea, because every amplitude is a function of these alone.
        bool operator==(const SeaState& other) const = default;

        /// The dispersion relation at this depth, `omega^2 = g k tanh(k h)`: a wave whose length
        /// approaches the depth falls behind deep water's `sqrt(g k)`, which is why a swell slows
        /// and steepens at a shore.
        float getFrequency(float wavenumber) const;

        /// The wavelength carrying the most energy, as an angular frequency.
        float getPeak() const { return getFrequency(Shaders::TAU / mPeakWavelength); }

        /// TMA's density at a frequency, in world units squared per radian a second.
        float getEnergy(float frequency) const;

        /// Donelan-Banner's width at a frequency: the spread is `sech^2(this * angle)`, so a large
        /// number is a narrow fan.
        float getSpread(float frequency) const;
    };
}
