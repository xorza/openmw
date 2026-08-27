#pragma once

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
}
