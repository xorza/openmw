#include "wavespectrum.hpp"

#include <algorithm>
#include <cmath>

namespace Rtx
{
    namespace
    {
        /// Donelan-Banner's spread parameter, against how far above the peak a band sits.
        ///
        /// **Large is narrow.** The swell arrives as near-parallel trains and comes out around two
        /// and a half; the chop well above the peak settles near four tenths, a fan wide enough that
        /// a sum of it does not draw a grain.
        float donelanSpread(float relative)
        {
            if (relative < 0.95f)
                return 2.61f * std::pow(relative, 1.3f);
            if (relative < 1.6f)
                return 2.28f * std::pow(relative, -1.3f);

            const float exponent = -0.4f + 0.8393f * std::exp(-0.567f * std::log(relative * relative));
            return std::pow(10.0f, exponent);
        }

        /// Kitaigorodskii's attenuation, which is what makes this TMA rather than JONSWAP.
        ///
        /// A shelf cannot carry a wave whose orbit reaches the bottom, so the spectrum is cut where
        /// the water is too shallow for it — from nothing, through a quadratic knee, to unchanged
        /// once the wave no longer feels the ground.
        float getDepthFactor(float frequency, float depth)
        {
            const float scaled = frequency * std::sqrt(depth / Shaders::WATER_GRAVITY);
            if (scaled <= 1.0f)
                return 0.5f * scaled * scaled;
            if (scaled < 2.0f)
                return 1.0f - 0.5f * (2.0f - scaled) * (2.0f - scaled);

            return 1.0f;
        }

        /// The TMA spectrum: JONSWAP shaped by how much of it a shelf this deep will carry.
        ///
        /// JONSWAP's `alpha` is left at one. It is a fetch-and-wind parameter nothing here knows and
        /// every term in it is a constant multiplier, so it cancels — the table is scaled to a
        /// significant height instead, which is the one number a person can picture.
        float tmaDensity(float frequency, float peak, float depth)
        {
            const float width = frequency <= peak ? 0.07f : 0.09f;
            const float offset = (frequency - peak) / (width * peak);
            const float sharpening = std::pow(3.3f, std::exp(-0.5f * offset * offset));
            const float ratio = peak / frequency;
            const float tail = std::exp(-1.25f * ratio * ratio * ratio * ratio);
            const float jonswap = Shaders::WATER_GRAVITY * Shaders::WATER_GRAVITY
                / (frequency * frequency * frequency * frequency * frequency) * tail * sharpening;

            return jonswap * getDepthFactor(frequency, depth);
        }
    }

    float SeaState::getFrequency(float wavenumber) const
    {
        return std::sqrt(Shaders::WATER_GRAVITY * wavenumber * std::tanh(wavenumber * mDepth));
    }

    float SeaState::getWavenumber(float frequency) const
    {
        const float target = frequency * frequency;
        float wavenumber = target / Shaders::WATER_GRAVITY;

        for (int step = 0; step < 12; ++step)
        {
            const float depth = wavenumber * mDepth;
            const float tanh = std::tanh(depth);
            const float value = Shaders::WATER_GRAVITY * wavenumber * tanh - target;
            const float slope
                = Shaders::WATER_GRAVITY * tanh + Shaders::WATER_GRAVITY * wavenumber * mDepth * (1.0f - tanh * tanh);
            wavenumber -= value / slope;
        }

        return std::max(wavenumber, 1.0e-6f);
    }

    float SeaState::getEnergy(float frequency) const
    {
        return tmaDensity(frequency, getPeak(), mDepth);
    }

    float SeaState::getSpread(float frequency) const
    {
        return donelanSpread(frequency / getPeak());
    }
}
