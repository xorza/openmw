#include "wavecascade.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "wavespectrum.hpp"

namespace Rtx
{
    namespace
    {
        /// Two uniform numbers in `(0, 1)` from a grid index, and the same two every time.
        ///
        /// **A sea has to be the same sea twice.** A cell reloaded, a screenshot taken again, or a
        /// test run on another machine all have to draw the same water, so the Gaussians below come
        /// off a hash of where they sit rather than out of a generator with a history.
        ///
        /// Wang's integer hash, which is enough for amplitudes nobody looks at individually.
        std::uint32_t scramble(std::uint32_t seed)
        {
            seed = (seed ^ 61u) ^ (seed >> 16u);
            seed *= 9u;
            seed = seed ^ (seed >> 4u);
            seed *= 0x27d4eb2du;
            return seed ^ (seed >> 15u);
        }

        /// A pair of standard normals, by Box-Muller off two hashes.
        ///
        /// **The whole of what makes a spectrum a sea rather than a shape.** An amplitude drawn at
        /// its expected value everywhere gives a surface whose every wave crests together, which is
        /// a pond in a cartoon; a Gaussian one gives the Rayleigh-distributed crest heights real
        /// water has, and the spectrum then states the *variance* rather than the surface.
        osg::Vec2f gaussians(std::uint32_t index)
        {
            // Away from zero on both, because the logarithm below is the one thing that cannot take
            // it. `scramble` reaches zero once in four thousand million and this costs nothing.
            const float first = (static_cast<float>(scramble(index) >> 8) + 0.5f) / 16777216.0f;
            const float second = (static_cast<float>(scramble(index ^ 0x9e3779b9u) >> 8) + 0.5f) / 16777216.0f;

            const float radius = std::sqrt(-2.0f * std::log(first));
            const float angle = Shaders::TAU * second;

            return osg::Vec2f(radius * std::cos(angle), radius * std::sin(angle));
        }

        /// How fast the dispersion relation carries a wavenumber into a frequency, `dw/dk`.
        ///
        /// **The Jacobian that turns a spectrum over frequency into one over wavevectors**, which is
        /// what a grid needs and what a list of bands never did. Differentiating
        /// `w^2 = g k tanh(k h)` gives it in closed form, so nothing here is a difference.
        float groupSlope(const SeaState& sea, float wavenumber)
        {
            const float depth = wavenumber * sea.mDepth;
            const float tanh = std::tanh(depth);
            const float frequency = sea.getFrequency(wavenumber);

            return Shaders::WATER_GRAVITY * (tanh + depth * (1.0f - tanh * tanh)) / (2.0f * frequency);
        }

        /// Donelan-Banner's density at an angle off the wind, normalised over the circle.
        ///
        /// `getSpread` states the width of a `sech^2`, and `SeaState::getWaves` samples the same
        /// shape by inverting its integral. A grid cannot invert anything — it has the angle
        /// already — so it needs the density itself, over the `2 tanh(s pi) / s` that shape covers.
        float spreadAt(float spread, float angle)
        {
            const float shape = 1.0f / std::cosh(spread * angle);

            return spread * shape * shape / (2.0f * std::tanh(spread * Shaders::PI));
        }
    }

    std::array<WaveCascade, Shaders::WAVE_CASCADES> makeWaveCascades(const SeaState& sea)
    {
        std::array<WaveCascade, Shaders::WAVE_CASCADES> cascades{};

        // Summed across every tile, because the significant height describes the surface and each
        // tile is an independent draw of a share of it.
        float variance = 0.0f;

        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            WaveCascade& cascade = cascades[index];
            cascade.mExtent = sWaveTiles[index].mExtent;
            cascade.mGrid = sWaveTiles[index].mGrid;

            const std::size_t count = cascade.mGrid * cascade.mGrid;
            const int half = static_cast<int>(cascade.mGrid) / 2;

            cascade.mAmplitudes.assign(count, osg::Vec2f());
            cascade.mFrequencies.assign(count, 0.0f);

            // What this tile can hold: a wave longer than its width is not periodic in it, and one
            // shorter than two of its texels is not there at all. Both tiles are wide enough and
            // fine enough for the whole spectrum, so what actually bounds the band is the spectrum's
            // own cutoff rather than either of these.
            const float longest = cascade.mExtent;
            const float shortest = std::max(sShortestWave, 2.0f * cascade.mExtent / static_cast<float>(cascade.mGrid));

            const float step = Shaders::TAU / cascade.mExtent;

            for (int row = 0; row < static_cast<int>(cascade.mGrid); ++row)
                for (int column = 0; column < static_cast<int>(cascade.mGrid); ++column)
                {
                    const osg::Vec2f wavevector(
                        step * static_cast<float>(column - half), step * static_cast<float>(row - half));

                    const float wavenumber = wavevector.length();
                    if (!(wavenumber > 0.0f))
                        continue;

                    const float wavelength = Shaders::TAU / wavenumber;
                    if (wavelength > longest || wavelength <= shortest)
                        continue;

                    const float frequency = sea.getFrequency(wavenumber);
                    const float angle = std::atan2(wavevector.y(), wavevector.x()) - sea.mBearing;

                    // The spectrum over wavevectors: the density over frequency, carried across by
                    // the dispersion relation's own slope, spread over directions, and divided by
                    // the wavenumber because `d2k` is `k dk dtheta`.
                    const float density = sea.getEnergy(frequency) * groupSlope(sea, wavenumber)
                        * spreadAt(sea.getSpread(frequency), std::remainder(angle, Shaders::TAU)) / wavenumber;

                    const std::size_t at
                        = static_cast<std::size_t>(row) * cascade.mGrid + static_cast<std::size_t>(column);

                    // **Half the density into each of the two Gaussians**, which is what makes the
                    // pair a circular complex normal: the real and imaginary parts are independent
                    // and equally strong, so the phase is uniform and the amplitude Rayleigh.
                    //
                    // And a share of the density per tile, because every tile carries the whole
                    // spectrum: independent draws of a fraction of the variance sum to one draw of
                    // all of it, which is what lets two periods stand in for none.
                    const float share = 1.0f / static_cast<float>(Shaders::WAVE_CASCADES);
                    const float scale = std::sqrt(0.5f * share * density * step * step);

                    // **A whole stream apart per tile, not a tile's worth.** The two grids are
                    // different sizes, so offsetting by a count would have the second tile draw the
                    // first's numbers wherever the two ran into each other — and two tiles carrying
                    // the same draws are one tile with the energy split, which is the one thing
                    // having two of them is for.
                    const std::uint32_t stream = static_cast<std::uint32_t>(index) * 0x51ed270bu;
                    cascade.mAmplitudes[at] = gaussians(stream + static_cast<std::uint32_t>(at)) * scale;
                    cascade.mFrequencies[at] = frequency;

                    // **Twice, because a wavevector and its opposite both carry it.** The field is
                    // `h0(k) e^{iwt} + conj(h0(-k)) e^{-iwt}`, and the two draws are independent, so
                    // the mean square of the sum is the sum of the two mean squares. Parseval then
                    // makes the surface's variance twice this sum — which is the convention
                    // `wavecompose.comp` is written against, and why no scale stands between them.
                    variance += 2.0f * cascade.mAmplitudes[at].length2();
                }
        }

        // Scaled to the height that was asked for, for the reason `getWaves` gives: JONSWAP's
        // `alpha` is a constant multiplier on everything here and cancels, so the one number a
        // person can picture takes its place.
        const float wanted = sea.mSignificantHeight / Shaders::WATER_SIGNIFICANT_HEIGHT;
        const float scale = variance > 0.0f ? wanted / std::sqrt(variance) : 0.0f;

        for (WaveCascade& cascade : cascades)
            for (osg::Vec2f& amplitude : cascade.mAmplitudes)
                amplitude *= scale;

        return cascades;
    }
}
