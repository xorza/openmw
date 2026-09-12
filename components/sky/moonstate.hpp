#pragma once

namespace Sky
{
    /// Where a moon stands and how much of it is lit, at one moment. `MWWorld::MoonModel` computes
    /// it and both renderers read it, which is why it is here rather than beside either.
    struct MoonState
    {
        /// **The order is the game's own and two things depend on it agreeing.** It is the order the
        /// eight `tx_masser_*` faces are listed in, and — because the steps are even — it is an
        /// angle: a phase is `index * 45 degrees` round the cycle, zero at full and 180 at new, so
        /// the lit share of the disc is `(1 + cos) / 2` and the sign of the sine says which limb
        /// keeps it.
        enum class Phase
        {
            Full,
            WaningGibbous,
            ThirdQuarter,
            WaningCrescent,
            New,
            WaxingCrescent,
            FirstQuarter,
            WaxingGibbous,
            Unspecified
        };

        static constexpr unsigned int phaseToInt(Phase phase)
        {
            switch (phase)
            {
                case Phase::New:
                    return 0;
                case Phase::WaxingCrescent:
                case Phase::WaningCrescent:
                    return 1;
                case Phase::FirstQuarter:
                case Phase::ThirdQuarter:
                    return 2;
                case Phase::WaxingGibbous:
                case Phase::WaningGibbous:
                    return 3;
                case Phase::Full:
                    return 4;
                case Phase::Unspecified:
                    return 0;
            }
            return 0;
        }

        float mRotationFromHorizon;
        float mRotationFromNorth;
        Phase mPhase;
        float mShadowBlend;
        float mMoonAlpha;

        /// What the hour alone fades the moon by, with no account of where it stands.
        ///
        /// **The half of `mMoonAlpha` that is about daylight.** `Moons_<name>_Fade_Out_*` takes the
        /// moons out between seven and ten in the morning and `Fade_In_*` brings them back between
        /// two and three in the afternoon; the other half hides a moon under
        /// `Moons_<name>_Fade_End_Angle`, which is the engine keeping a lit quad off its own fogged
        /// horizon. A renderer with air in it wants the first without the second.
        float mDaylightFade;
    };

    using MoonPhase = MoonState::Phase;

    constexpr unsigned int phaseToInt(MoonPhase phase)
    {
        return MoonState::phaseToInt(phase);
    }
}
