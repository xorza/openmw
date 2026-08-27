#ifndef OPENMW_COMPONENTS_SCENEUTIL_LIGHTCONTROLLER_H
#define OPENMW_COMPONENTS_SCENEUTIL_LIGHTCONTROLLER_H

namespace SceneUtil
{

    /// @brief How bright a light is at one instant, as a multiplier on what it radiates.
    ///
    /// A `LIGH` record says *that* a light flickers or pulses and never says how: it carries a
    /// colour, a radius and four flags, and no amplitude, rate or phase anywhere. So every number
    /// behind this is chosen here, and each says what it was chosen from.
    ///
    /// **A function of the clock, and of nothing else.** The animation keeps no state that a frame
    /// advances, which is what makes it the same at a given instant however it is reached: at any
    /// frame rate, from any renderer, and however many times one frame asks. What separates two
    /// candles standing together is a phase drawn once, when the light is built.
    class LightController
    {
    public:
        /// What the record's flags say the light does. One at most — no record in the game carries
        /// two, and `Flicker` beside `PulseSlow` would mean nothing if one did.
        enum LightType
        {
            LT_Normal,
            LT_Flicker,
            LT_FlickerSlow,
            LT_Pulse,
            LT_PulseSlow
        };

        LightController();

        void setType(LightType type) { mType = type; }

        /// What to multiply the light's colour by, at `simulationTime` seconds.
        ///
        /// Lands in `1 +- depth` and averages exactly one over time, so a light that animates is as
        /// bright on average as the same light standing still. See `sFlameDepth`.
        float brightnessAt(double simulationTime) const;

    private:
        /// One sine of the ladder: `index` steps this light's phase along, `frequency` is in hertz.
        float band(double simulationTime, float frequency, int index) const;

        /// The sum of four bands of the ladder, the highest of them at `top` hertz, in `-1 .. 1`.
        float flame(double simulationTime, float top) const;

        LightType mType;

        /// Where this light stands in its animation, in turns, drawn when it was built.
        float mPhase;
    };

}

#endif
