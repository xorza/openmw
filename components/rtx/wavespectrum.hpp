#pragma once

#include "shaders/scene.h"

namespace Rtx
{
    /// The shortest wave the spectrum carries, in world units — a band limit in time as much as in
    /// space, because the shortest waves decide the caustics and how fast they reshuffle. Carried
    /// down to eighteen units the seabed read as stripes tearing rather than as water.
    inline constexpr float sShortestWave = 32.0f;

    /// What the sea is doing, in the four numbers a spectrum needs. TMA — JONSWAP under
    /// Kitaigorodskii's shallow-water attenuation — spread over directions by Donelan-Banner, the
    /// pairing Horvath's *Empirical Directional Wave Spectra for Computer Graphics* settled on:
    /// TMA's depth term is the coastal-shelf correction this water needs, and a
    /// frequency-dependent spread is what a sum of plane waves needs if it is not to draw a
    /// lattice. `makeWaveCascades` lays it on a grid once, on the host.
    struct SeaState
    {
        /// The average height of the highest third of the waves, in world units. The figure
        /// oceanography quotes, and the one that decides how rough this looks.
        float mSignificantHeight = 9.4f;

        /// The wavelength carrying the most energy, in world units.
        float mPeakWavelength = 420.0f;

        /// Depth of the shelf the spectrum is attenuated against.
        float mDepth = 300.0f;

        /// What decides whether the amplitudes have to be drawn again. Every one of them is a
        /// function of these four numbers and of nothing else, so two equal states are one sea.
        bool operator==(const SeaState& other) const = default;

        /// The dispersion relation at this depth: `omega^2 = g k tanh(k h)`. Deep water's
        /// `sqrt(g k)` is only its limit, and a wave whose length approaches the depth falls behind
        /// it — which is why a swell slows and steepens as it reaches a shore.
        float getFrequency(float wavenumber) const;

        /// The same relation the other way round, by Newton from the deep-water guess.
        float getWavenumber(float frequency) const;

        /// The wavelength carrying the most energy, as an angular frequency.
        float getPeak() const { return getFrequency(Shaders::TAU / mPeakWavelength); }

        /// TMA's density at a frequency, in world units squared per radian a second, shared with
        /// the cascades.
        float getEnergy(float frequency) const;

        /// Donelan-Banner's width at a frequency: the spread is `sech^2(this * angle)` normalised
        /// over the circle, so a large number is a narrow fan.
        float getSpread(float frequency) const;
    };
}
