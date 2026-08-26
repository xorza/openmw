#include "wavespectrum.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Rtx
{
    namespace
    {
        /// How many wavenumber bands the spectrum is sampled at, and how many directions each takes.
        constexpr std::size_t sBands = 8;
        constexpr std::size_t sPerBand = Shaders::WAVE_COUNT / sBands;

        /// The golden angle, turning each band off the last so no two share a direction.
        constexpr float sGolden = 2.3999632f;

        /// The shortest wave the spectrum carries, in world units.
        ///
        /// **A band limit in time as much as in space.** Curvature climbs with wavenumber, so the
        /// shortest waves decide the caustics — and a wave's period falls with its length, so they
        /// also decide how fast the pattern on a seabed reshuffles. Carried down to eighteen units
        /// the light below changed by three quarters of its own contrast every twelfth of a second,
        /// which reads as stripes tearing across the bottom rather than as water. Thirty-two puts
        /// that back to half, and costs a quarter of the contrast to do it.
        ///
        /// The trade is exactly that and cannot be had both ways: shorter waves focus harder *and*
        /// move faster, because they are the same waves.
        ///
        /// Where the bands are *centred*, not where they end: the lowest sits below the peak and the
        /// highest half a band above this, so the table spans about 980 down to 27 units.
        constexpr float sShortest = 32.0f;

        /// Donelan-Banner's spread parameter, against how far above the peak a band sits.
        ///
        /// **Large is narrow.** The swell arrives as near-parallel trains and comes out around two
        /// and a half; the chop well above the peak settles near four tenths, a fan wide enough that
        /// a sum of it does not draw a grain.
        float getSpread(float relative)
        {
            if (relative < 0.95f)
                return 2.61f * std::pow(relative, 1.3f);
            if (relative < 1.6f)
                return 2.28f * std::pow(relative, -1.3f);

            const float exponent = -0.4f + 0.8393f * std::exp(-0.567f * std::log(relative * relative));
            return std::pow(10.0f, exponent);
        }

        /// Where a given share of a `sech^2` spread lies, in radians off the wind.
        ///
        /// The spread integrates to `tanh`, so its quantiles are an `atanh` — no search, no table.
        float getTurn(float quantile, float spread)
        {
            const float edge = std::tanh(spread * Shaders::PI);
            return std::atanh((2.0f * quantile - 1.0f) * edge) / spread;
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
        float getEnergy(float frequency, float peak, float depth)
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

    std::array<Shaders::GpuWave, Shaders::WAVE_COUNT> SeaState::getWaves() const
    {
        const float peak = getFrequency(Shaders::TAU / mPeakWavelength);

        // From below the peak, where the swell is, up to the shortest wave worth carrying.
        const float lowest = 0.7f * peak;
        const float highest = getFrequency(Shaders::TAU / sShortest);
        const float step = std::pow(highest / lowest, 1.0f / static_cast<float>(sBands - 1));

        std::array<Shaders::GpuWave, Shaders::WAVE_COUNT> waves{};

        for (std::size_t band = 0; band < sBands; ++band)
        {
            const float frequency = lowest * std::pow(step, static_cast<float>(band));

            // The band's width, taken symmetrically about it in the geometric spacing.
            const float width = frequency * (std::sqrt(step) - 1.0f / std::sqrt(step));
            const float energy = getEnergy(frequency, peak, mDepth) * width / static_cast<float>(sPerBand);
            const float spread = getSpread(frequency / peak);

            for (std::size_t within = 0; within < sPerBand; ++within)
            {
                const float quantile = (static_cast<float>(within) + 0.5f) / static_cast<float>(sPerBand);
                const float angle = mBearing + sGolden * static_cast<float>(band) + getTurn(quantile, spread);

                // **Each direction takes its own place in the band, not the band's middle.** A band
                // stands for a range of wavelengths, and giving all of its directions the same one
                // gives them the same speed too — plane waves of a single length travelling
                // together do not interfere into a sea, they translate as a rigid pattern, and the
                // seabed below reads that as stripes sliding across it. Spread across the band they
                // beat against one another instead, which is the whole point of a spectrum.
                const float own = frequency * std::pow(step, quantile - 0.5f);

                waves[band * sPerBand + within] = Shaders::GpuWave{
                    .mDirection = osg::Vec2f(std::cos(angle), std::sin(angle)),
                    .mWavenumber = getWavenumber(own),
                    // Held back until the whole table is known, so it can be scaled to the height
                    // that was actually asked for.
                    .mAmplitude = std::sqrt(std::max(2.0f * energy, 0.0f)),
                    .mSpeed = own,
                };
            }
        }

        // **The spectrum's own scale is thrown away and the asked-for height put in its place**, for
        // the reason `getEnergy` gives: everything `alpha` contributes is a constant multiplier.
        float variance = 0.0f;
        for (const Shaders::GpuWave& wave : waves)
            variance += wave.mAmplitude * wave.mAmplitude * 0.5f;

        if (variance > 0.0f)
        {
            const float scale = mSignificantHeight / (Shaders::WATER_SIGNIFICANT_HEIGHT * std::sqrt(variance));
            for (Shaders::GpuWave& wave : waves)
                wave.mAmplitude *= scale;
        }

        return waves;
    }
}
