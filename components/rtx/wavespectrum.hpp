#pragma once

#include "shaders/scene.h"

namespace Rtx
{
    /// The shortest wave the spectrum carries, in world units.
    ///
    /// **A band limit in time as much as in space.** Curvature climbs with wavenumber, so the
    /// shortest waves decide the caustics — and a wave's period falls with its length, so they also
    /// decide how fast the pattern on a seabed reshuffles. Carried down to eighteen units the light
    /// below changed by three quarters of its own contrast every twelfth of a second, which reads as
    /// stripes tearing across the bottom rather than as water.
    ///
    /// The trade is exactly that and cannot be had both ways: shorter waves focus harder *and* move
    /// faster, because they are the same waves. What softened it is the transform: spread over tens
    /// of thousands of wavevectors rather than sixty-four, the curvature at this cutoff reshuffles a
    /// quarter of the pattern in a twelfth of a second where the sinusoid table reshuffled half.
    inline constexpr float sShortestWave = 32.0f;

    /// What the sea is doing, in the four numbers a spectrum needs.
    ///
    /// What decides whether a surface looks like water is how its energy is laid out over
    /// wavelengths and directions. This is the curve that says so; `makeWaveCascades` lays it on a
    /// grid of wavevectors, once, on the host — nothing here runs per pixel.
    ///
    /// The spectrum is **TMA**: JONSWAP under Kitaigorodskii's shallow-water attenuation, spread
    /// over directions by **Donelan-Banner**, which is the pairing Horvath's *Empirical Directional
    /// Wave Spectra for Computer Graphics* settled on. Two things earn it over a geometric series
    /// picked by eye: TMA's depth term is exactly the coastal-shelf correction this game's water
    /// needs, and Donelan-Banner's spread is frequency-dependent — narrow at the swell, broad at the
    /// chop — which is the shape a sum of plane waves must have if it is not to draw a lattice.
    struct SeaState
    {
        /// The average height of the highest third of the waves, in world units. The figure
        /// oceanography quotes, and the one that decides how rough this looks.
        float mSignificantHeight = 9.4f;

        /// The wavelength carrying the most energy, in world units.
        float mPeakWavelength = 420.0f;

        /// Depth of the shelf the spectrum is attenuated against.
        float mDepth = 300.0f;

        /// Which way the wind blows, in radians about +Z.
        float mBearing = 0.6f;

        /// **What decides whether the amplitudes have to be drawn again.** Every one of them is a
        /// function of these four numbers and of nothing else, so two equal states are one sea.
        bool operator==(const SeaState& other) const = default;

        /// The dispersion relation at this depth: `omega^2 = g k tanh(k h)`.
        ///
        /// Deep water's `sqrt(g k)` is only its limit, and a wave whose length approaches the depth
        /// falls behind it — which is why a swell slows and steepens as it reaches a shore.
        float getFrequency(float wavenumber) const;

        /// The same relation the other way round, by Newton from the deep-water guess.
        float getWavenumber(float frequency) const;

        /// The wavelength carrying the most energy, as an angular frequency.
        float getPeak() const { return getFrequency(Shaders::TAU / mPeakWavelength); }

        /// TMA's density at a frequency, in world units squared per radian a second.
        ///
        /// **Shared with the cascades**, which need the same spectrum laid out on a grid of
        /// wavevectors rather than sampled at bands. Two evaluations of one curve is two curves the
        /// day one of them is edited.
        float getEnergy(float frequency) const;

        /// Donelan-Banner's width at a frequency: how tightly that band fans about the wind.
        ///
        /// The spread is `sech^2(this * angle)` normalised over the circle, so a large number is a
        /// narrow fan. Narrow at the swell and broad at the chop, which is the shape a sea has to
        /// have if it is not to draw a lattice.
        float getSpread(float frequency) const;
    };
}
