#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <osg/Vec2f>

#include <components/rtx/shaders/scene.h>
#include <components/rtx/wavecascade.hpp>

namespace Rtx::Testing
{
    /// The wavevector the entry at `at` carries, in radians a world unit.
    ///
    /// **Written out on this side rather than taken off the builder**, which is what every helper
    /// here is for: a half-grid shift dropped there has to fail these tests rather than pass them.
    inline osg::Vec2f waveVectorAt(const WaveCascade& cascade, std::size_t at)
    {
        const float step = Shaders::TAU / cascade.mExtent;
        const int half = static_cast<int>(cascade.mGrid) / 2;
        const int row = static_cast<int>(at / cascade.mGrid) - half;
        const int column = static_cast<int>(at % cascade.mGrid) - half;

        return osg::Vec2f(step * static_cast<float>(column), step * static_cast<float>(row));
    }

    /// A moment of the surface a tile stands for, summed over its own amplitudes.
    ///
    /// **Parseval.** Each wavevector contributes `2 |h0|^2` of elevation variance — the field is
    /// `h0(k) e^{iwt} + conj(h0(-k)) e^{-iwt}` and the two draws are independent — and a derivative
    /// multiplies that by a power of the wavenumber. So the same sum with `power` at 0, 2 and 4 is
    /// the mean square of the elevation, of the slope and of the curvature's trace.
    inline float momentOf(const WaveCascade& cascade, int power)
    {
        float total = 0.0f;
        for (std::size_t at = 0; at < cascade.mAmplitudes.size(); ++at)
        {
            const float squared = waveVectorAt(cascade, at).length2();
            total += 2.0f * cascade.mAmplitudes[at].length2() * std::pow(squared, 0.5f * static_cast<float>(power));
        }

        return total;
    }

    /// How much of the slope a mip chain read at `footprint` has averaged away.
    ///
    /// **What `WaterSurface::mLostSlope` is, written out on this side.** A level of the chain is the
    /// mean of a square of texels, and a mean of `n` *samples* a texel apart is Dirichlet's kernel
    /// rather than a `sinc`: the field was point-sampled onto the grid before any of it was
    /// averaged, so what a level passes is `sin(n p) / (n sin p)` with `p` half a wavevector's phase
    /// across one texel. Reading it as the box of a continuous field over-states the loss by a fifth.
    /// What survives is the field through that, what is lost is the rest, and the shader takes the
    /// difference of a mean square and a squared mean to get it.
    ///
    /// **The blend of two levels, because `textureLod` reads at a fraction.** `waveLevel` asks for
    /// `log2(footprint / texel)`, and the sampler mixes the levels either side of it — so the field
    /// the shader sees is the mix of two boxes and its transfer is the mix of their two, not the box
    /// of any single width.
    ///
    /// **Never below the finest level**, which is where `waveLevel` clamps: a cone narrower than a
    /// texel has nothing further to be shown, and the chain has nothing finer to show it.
    inline float lostSlopeOf(const WaveCascade& cascade, float footprint)
    {
        const float texel = cascade.mExtent / static_cast<float>(cascade.mGrid);
        const float level = std::max(std::log2(footprint / texel), 0.0f);
        const float share = level - std::floor(level);

        const auto passed = [texel](float count, const osg::Vec2f& wavevector) {
            const auto along = [texel, count](float wavenumber) {
                const float phase = 0.5f * wavenumber * texel;

                return std::abs(std::sin(phase)) < 1e-6f ? 1.0f : std::sin(count * phase) / (count * std::sin(phase));
            };

            return along(wavevector.x()) * along(wavevector.y());
        };

        const float coarse = std::exp2(std::floor(level));
        const float finer = std::exp2(std::floor(level) + 1.0f);

        float lost = 0.0f;
        for (std::size_t at = 0; at < cascade.mAmplitudes.size(); ++at)
        {
            const osg::Vec2f wavevector = waveVectorAt(cascade, at);
            const float carried = (1.0f - share) * passed(coarse, wavevector) + share * passed(finer, wavevector);

            lost += 2.0f * cascade.mAmplitudes[at].length2() * wavevector.length2() * (1.0f - carried * carried);
        }

        return lost;
    }
}
