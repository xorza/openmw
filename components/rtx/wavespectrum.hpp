#pragma once

#include <array>

#include "shaders/scene.h"

namespace Rtx
{
    /// What the sea is doing, in the four numbers a spectrum needs.
    ///
    /// The surface is a sum of plane waves, and what decides whether it looks like water is which
    /// waves are in the sum. This turns a sea state into that list, once, on the host — nothing here
    /// runs per pixel.
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

        /// The sinusoids this sea is made of.
        ///
        /// Sampled by *quantile* in direction rather than at even angles: the share of a band's
        /// energy between two quantiles of its spread is the same by construction, so every
        /// component carries the same amplitude and the spread's shape is exact however few
        /// directions are taken.
        std::array<Shaders::GpuWave, Shaders::WAVE_COUNT> getWaves() const;

        /// The dispersion relation at this depth: `omega^2 = g k tanh(k h)`.
        ///
        /// Deep water's `sqrt(g k)` is only its limit, and a wave whose length approaches the depth
        /// falls behind it — which is why a swell slows and steepens as it reaches a shore.
        float getFrequency(float wavenumber) const;

        /// The same relation the other way round, by Newton from the deep-water guess.
        float getWavenumber(float frequency) const;
    };
}
