#include "lightcontroller.hpp"

#include <cmath>

#include <osg/Math>

#include <components/misc/rng.hpp>

namespace SceneUtil
{
    namespace
    {
        /// The top of the ladder every animation is built from, in hertz.
        ///
        /// **A flame's puffing frequency, capped by what one sample a frame can carry.** A buoyant
        /// diffusion flame sheds a vortex ring at about `1.5 / sqrt(D)` hertz, with `D` its width
        /// in metres, so a lamp flame near 28 mm across puffs at nine. Nine is also as high as this
        /// can usefully reach: the light is read once a frame, which is 6.7 samples a cycle at 60
        /// frames a second and 3.3 at 30. Anything faster reads as noise at the first rate and
        /// aliases into a slower beat at the second.
        constexpr float sTopBand = 9.0f;

        /// One step down the ladder of bands, and the step between a fast animation and its slow
        /// twin.
        ///
        /// **The golden ratio squared, because it is irrational.** Bands at a rational ratio come
        /// back into phase and the whole flicker repeats on that period, which a viewer standing
        /// still in a lit room sees. These never do.
        constexpr float sBandRatio = 2.618034f;

        /// How many bands a flame is the sum of. Four spans a factor of eighteen in rate, which is
        /// the whole of what a flame does: the puffing at the top, and a draught wandering under it.
        constexpr int sFlameBands = 4;

        /// How far a flame swings, as a fraction of what the light radiates.
        ///
        /// This is the peak, and the bands are weighted to sum to one, so the brightness lands in
        /// `1 +- sFlameDepth` exactly. What it usually is is far smaller: with equal bands the
        /// deviation is `sFlameDepth / sqrt(2 * sFlameBands)` RMS, which is 11% of the light. A
        /// candle burning in still air varies by about a tenth of its output, and a peak three
        /// times that is the draught.
        constexpr float sFlameDepth = 0.30f;

        /// How far a pulse swings. Deeper than a flame, because a pulse is the whole of what the
        /// light does: the content gives it to lava, to glowing lichen, to Dwemer tubes and to
        /// enchanted rings, and none of those has a flame for it to be a variation of.
        constexpr float sPulseDepth = 0.35f;

        /// The slow pulse, in hertz. Three seconds a cycle reads as a swell rather than as a
        /// flicker, which is the whole difference between the two kinds.
        constexpr float sPulseBand = 1.0f / 3.0f;

        /// How far apart one light's bands are set, in turns. The golden ratio's conjugate spreads
        /// any number of them around the circle without two landing together.
        constexpr float sBandPhase = 0.618034f;
    }

    LightController::LightController()
        : mType(LT_Normal)
        , mPhase(Misc::Rng::rollClosedProbability())
    {
    }

    float LightController::band(double simulationTime, float frequency, int index) const
    {
        // **Reduced to one turn in double, before it is narrowed.** A session's clock reaches tens
        // of thousands of seconds, and a float holding that many turns at nine hertz has nothing
        // left for the fraction of a turn that is the whole answer.
        const auto turns = static_cast<float>(std::fmod(frequency * simulationTime, 1.0));

        return std::sin(2.0f * osg::PIf * (turns + mPhase + static_cast<float>(index) * sBandPhase));
    }

    float LightController::flame(double simulationTime, float top) const
    {
        float sum = 0.0f;
        float frequency = top;

        for (int i = 0; i < sFlameBands; ++i)
        {
            sum += band(simulationTime, frequency, i);
            frequency /= sBandRatio;
        }

        // **Equal weights, which is what makes the spectrum pink.** The bands are a geometric
        // ladder, so one weight each is one share of the power per octave — the spectrum a flame
        // has, and the reason this reads as a flame rather than as a wobble at one rate. Divided by
        // their count so that the sum cannot leave `-1 .. 1`, which is what bounds the brightness.
        return sum / static_cast<float>(sFlameBands);
    }

    float LightController::brightnessAt(double simulationTime) const
    {
        switch (mType)
        {
            case LT_Normal:
                return 1.0f;
            case LT_Flicker:
                // The whole flame, puffing included. The content gives this one to open fires: a
                // tiki torch, a brazier, a spark shower and a failing Dwemer tube.
                return 1.0f + sFlameDepth * flame(simulationTime, sTopBand);
            case LT_FlickerSlow:
                // **The same flame with its puffing damped away**, which is what a flame behind
                // lantern glass, high on a wall or across a room actually shows: the drift is left,
                // and one slower band arrives under it. The window down the ladder is the whole
                // difference between the two, and it is what makes this one cross its own mean
                // about a third as often.
                return 1.0f + sFlameDepth * flame(simulationTime, sTopBand / sBandRatio);
            case LT_Pulse:
                return 1.0f + sPulseDepth * band(simulationTime, sPulseBand * sBandRatio, 0);
            case LT_PulseSlow:
                return 1.0f + sPulseDepth * band(simulationTime, sPulseBand, 0);
        }

        return 1.0f;
    }

}
