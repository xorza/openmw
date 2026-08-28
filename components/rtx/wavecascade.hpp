#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <osg/Vec2f>

#include "shaders/wave.h"

namespace Rtx
{
    struct SeaState;

    /// How wide a tile is and how finely it is sampled, before any amplitudes are drawn.
    struct WaveTile
    {
        /// World units across. Its wavevectors are multiples of `TAU / mExtent`.
        float mExtent;

        /// Samples along each axis. It reaches from `mExtent` down to two of its own texels.
        std::size_t mGrid;
    };

    /// The tiles the sea is summed from.
    ///
    /// **The widths are not multiples of one another, which is the whole point of having two.**
    /// Every tile lays the same water down at its own period, and periods that divide into each
    /// other line up and draw one coarse grid over the sea. These two stand in the ratio 2.696, so
    /// they come back into step only after tens of tiles — past anything a frame contains.
    ///
    /// **Both hold the whole spectrum**, which is what lets them share it rather than divide it.
    /// Each is wide enough for the longest wave — ten of the peak fit across the larger — and fine
    /// enough for the shortest, whose Nyquist is sixteen units against the thirty-two the spectrum
    /// stops at.
    ///
    /// **The grids are sized to the band and not to the tile.** The spectrum occupies only the part
    /// of a grid's reach above `sShortestWave`, so a tile a third as wide needs a quarter of the
    /// grid to hold the same wavelengths at the same density. One size for both spent three quarters
    /// of the narrow tile's transform on cells the spectrum never reaches: 7088 live of 262144.
    inline constexpr std::array<WaveTile, Shaders::WAVE_CASCADES> sWaveTiles{
        WaveTile{ .mExtent = 4096.0f, .mGrid = Shaders::WAVE_GRID },
        WaveTile{ .mExtent = 1519.0f, .mGrid = 128 },
    };

    /// One tile of the sea, as the complex amplitudes an inverse transform turns into a field.
    ///
    /// **Built once for a sea state and never per frame.** What a frame does with this is turn its
    /// phases — `h(k, t)` is `h0(k)` times a rotation — which is a multiply, where deriving the
    /// amplitudes is a spectrum evaluation and a Gaussian draw for every one of sixteen thousand
    /// wavevectors.
    struct WaveCascade
    {
        /// How wide the tile is, in world units. Its wavevectors are multiples of `TAU / mExtent`.
        float mExtent = 0.0f;

        /// How many samples across this tile is transformed on, from `sWaveTiles`.
        std::size_t mGrid = 0;

        /// `mGrid` squared complex amplitudes, row major.
        ///
        /// Entry `row * mGrid + column` carries the wavevector
        /// `TAU / mExtent * (column - mGrid / 2, row - mGrid / 2)`, which is the layout an inverse
        /// transform reads and puts the long waves in the middle rather than at the corner.
        ///
        /// **Not conjugate-symmetric, and it must not be.** Symmetry is what makes the *field* real,
        /// and the field is `h0(k) e^{iwt} + conj(h0(-k)) e^{-iwt}` — a sum this pair of entries is
        /// built to satisfy at every time rather than at one. Storing half of it and mirroring would
        /// give a real surface that could not move.
        std::vector<osg::Vec2f> mAmplitudes;

        /// Radians a second, one per entry, in the same order. `omega(k)` off the sea's own
        /// dispersion relation, so a shallow shelf slows its long waves here as it does everywhere.
        std::vector<float> mFrequencies;
    };

    /// The tiles a sea state comes to.
    ///
    /// **Scaled together rather than each to itself**, because the significant height is a property
    /// of the whole surface: the tiles are independent draws whose variances add, and normalising
    /// one at a time would give two seas of the roughness asked for rather than one.
    std::array<WaveCascade, Shaders::WAVE_CASCADES> makeWaveCascades(const SeaState& sea);

    /// How many levels a tile transformed on this grid has, counting down to the single texel that
    /// makes the last level the tile's own mean.
    inline std::uint32_t levelsFor(std::size_t grid)
    {
        return static_cast<std::uint32_t>(std::bit_width(grid));
    }

    /// How much curvature these tiles carry, and how much of it survives each level of their chains.
    ///
    /// **The caustic's own normaliser, and it is a property of the sea rather than of a place.**
    /// `causticGain` is the mean of the estimator conditioned on how far the map has folded, so the
    /// fold handed to it has to be an ensemble quantity. Differenced out of the chain per pixel it
    /// was neither, and it answered for the wrong field besides — see `mWaveResolved`.
    struct WaveCurvature
    {
        /// Mean square of the curvature's trace over the whole spectrum. What the caustic's `bend` is
        /// sized against, so that one number sets how far the map runs whatever the sea state is.
        float mWhole = 0.0f;

        /// What share of `mWhole` a tile still resolves at a level of its own chain, indexed
        /// `cascade * WAVE_LEVELS + level`. The shares sum to one across the tiles at the finest
        /// level, and toward nought at the coarsest, where the surface has been averaged flat.
        std::array<float, Shaders::WAVE_CASCADES * Shaders::WAVE_LEVELS> mResolved{};
    };

    /// What a mip chain over these tiles leaves of their curvature.
    ///
    /// **Two filters, because a sampler is one as well as the chain.** A level is a mean of point
    /// samples, so its own transfer is Dirichlet's kernel and not a `sinc` — the field is sampled
    /// onto the grid before any of it is averaged, and reading a level as the box of a continuous
    /// field over-states what it took by a fifth. Then `textureLod` reconstructs between those
    /// samples bilinearly, which passes `(2 + cos(k w)) / 3` of a frequency's power over tap
    /// positions spread through a texel of width `w`: one at the long end of the band and a third at
    /// its Nyquist. That second filter is worth a quarter of the curvature at the finest level and
    /// two thirds of it at the levels deep water reads.
    ///
    /// **Off the amplitudes that were drawn**, for the reason `waveSlope` gives: a tile that dropped
    /// a band for want of grid says so here too.
    WaveCurvature waveCurvature(const std::array<WaveCascade, Shaders::WAVE_CASCADES>& cascades);

    /// Root mean square slope of the surface these tiles describe, over every wavelength in them.
    ///
    /// **Parseval, over the amplitudes rather than over the field.** A slope is the elevation
    /// differentiated once, so each wavevector contributes its own variance weighted by the square
    /// of its wavenumber — and the draws at `k` and `-k` are independent, which is the factor of two.
    /// It is the same sum the coarsest level of the curvature chain carries, computed here because a
    /// mip chain is a backend's answer and this is a property of the sea.
    ///
    /// **Off the amplitudes that were drawn and not off the spectrum they came from**, so a tile
    /// that dropped a band for want of grid says so here too.
    float waveSlope(const std::array<WaveCascade, Shaders::WAVE_CASCADES>& cascades);

}
