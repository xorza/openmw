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

    /// The tiles the sea is summed from. The widths stand in the ratio 2.696, because periods that
    /// divide into each other line up and draw one coarse grid over the sea. Both hold the whole
    /// spectrum: each is wide enough for the longest wave and fine enough for the shortest. The
    /// grids are sized to the band and not to the tile, because one size for both spent three
    /// quarters of the narrow tile's transform on cells the spectrum never reaches.
    inline constexpr std::array<WaveTile, Shaders::WAVE_CASCADES> sWaveTiles{
        WaveTile{ .mExtent = 4096.0f, .mGrid = Shaders::WAVE_GRID },
        WaveTile{ .mExtent = 1519.0f, .mGrid = 128 },
    };

    /// One tile of the sea, as the complex amplitudes an inverse transform turns into a field.
    /// Built once for a sea state: a frame turns its phases, which is a multiply, where deriving
    /// the amplitudes is a spectrum evaluation and a Gaussian draw per wavevector.
    struct WaveCascade
    {
        /// How wide the tile is, in world units. Its wavevectors are multiples of `TAU / mExtent`.
        float mExtent = 0.0f;

        /// How many samples across this tile is transformed on, from `sWaveTiles`.
        std::size_t mGrid = 0;

        /// `mGrid` squared complex amplitudes, row major: entry `row * mGrid + column` carries the
        /// wavevector `TAU / mExtent * (column - mGrid / 2, row - mGrid / 2)`. Not
        /// conjugate-symmetric, because the field is `h0(k) e^{iwt} + conj(h0(-k)) e^{-iwt}`, and
        /// storing half of it and mirroring would give a real surface that could not move.
        std::vector<osg::Vec2f> mAmplitudes;

        /// Radians a second, one per entry, in the same order. `omega(k)` off the sea's own
        /// dispersion relation, so a shallow shelf slows its long waves here as it does everywhere.
        std::vector<float> mFrequencies;
    };

    /// The tiles a sea state comes to, scaled together rather than each to itself, because the
    /// tiles are independent draws whose variances add.
    std::array<WaveCascade, Shaders::WAVE_CASCADES> makeWaveCascades(const SeaState& sea);

    /// How many levels a tile transformed on this grid has, counting down to the single texel that
    /// makes the last level the tile's own mean.
    inline std::uint32_t levelsFor(std::size_t grid)
    {
        return static_cast<std::uint32_t>(std::bit_width(grid));
    }

    /// How much curvature these tiles carry, and how much of it survives each level of their chains
    /// — the caustic's own normaliser, a property of the sea rather than of a place, because
    /// `causticGain` is the mean of the estimator conditioned on how far the map has folded.
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

    /// What a mip chain over these tiles leaves of their curvature. Two filters, because a sampler
    /// is one as well as the chain: a level is a mean of point samples, so its transfer is
    /// Dirichlet's kernel and not a `sinc`, and `textureLod` then reconstructs bilinearly, passing
    /// `(2 + cos(k w)) / 3` of a frequency's power — a quarter of the curvature at the finest level
    /// and two thirds at the levels deep water reads. Off the amplitudes that were drawn, as
    /// `waveSlope` is.
    WaveCurvature waveCurvature(const std::array<WaveCascade, Shaders::WAVE_CASCADES>& cascades);

    /// Root mean square slope of the surface these tiles describe: Parseval over the amplitudes,
    /// each wavevector's variance weighted by the square of its wavenumber, and the draws at `k`
    /// and `-k` independent, which is the factor of two. Off the amplitudes that were drawn and not
    /// off the spectrum, so a tile that dropped a band for want of grid says so here too.
    float waveSlope(const std::array<WaveCascade, Shaders::WAVE_CASCADES>& cascades);

}
