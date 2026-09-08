#pragma once

#include <string_view>

namespace Sky
{
    /// The eight phases a moon is painted in, counted from full.
    ///
    /// **The order is the game's own and two things depend on it agreeing.** It is the order the
    /// eight `tx_masser_*` faces are listed in, and — because the steps are even — it is an angle:
    /// a phase is `index * 45 degrees` round the cycle, zero at full and 180 at new, so the lit
    /// share of the disc is `(1 + cos) / 2` and the sign of the sine says which limb keeps it.
    enum class MoonPhase
    {
        Full,
        WaningGibbous,
        ThirdQuarter,
        WaningCrescent,
        New,
        WaxingCrescent,
        FirstQuarter,
        WaxingGibbous,
    };

    /// Where a moon stands and how much of it is lit, at one moment.
    /// Which of the five faces a phase is drawn with, waxing and waning sharing one.
    ///
    /// **Beside the enum rather than in a renderer's struct**, because it is a fact about the eight
    /// phases and not about either renderer.
    constexpr unsigned int phaseToInt(MoonPhase phase)
    {
        switch (phase)
        {
            case MoonPhase::New:
                return 0;
            case MoonPhase::WaxingCrescent:
            case MoonPhase::WaningCrescent:
                return 1;
            case MoonPhase::FirstQuarter:
            case MoonPhase::ThirdQuarter:
                return 2;
            case MoonPhase::WaxingGibbous:
            case MoonPhase::WaningGibbous:
                return 3;
            case MoonPhase::Full:
                return 4;
        }

        return 0;
    }

    struct MoonMoment
    {
        /// Degrees travelled from the horizon it rose at, zero to 180 — and zero again once it has
        /// set, which is the engine's own way of saying it is not up.
        float mAlongArc = 0.0f;

        /// Degrees the whole arc is swung about the zenith. **Not a tilt**: swinging keeps both
        /// moons climbing as high as the sun does and moves only where they rise, which is what
        /// makes their paths cross.
        float mAxisOffset = 0.0f;

        MoonPhase mPhase = MoonPhase::Full;

        /// How much of the textured face shows against a disc the colour of the sky, which is what
        /// the original engine crossfaded near the horizon.
        float mShadowBlend = 0.0f;

        /// What the hour and the arc between them fade the moon by. Zero is a moon that is not
        /// drawn at all, whether or not it is above the horizon.
        float mAlpha = 0.0f;

        /// What the hour alone fades it by, with no account of where it stands.
        ///
        /// **The half of `mAlpha` that is about daylight.** `Moons_<name>_Fade_Out_*` takes the moons
        /// out between seven and ten in the morning and `Fade_In_*` brings them back between two and
        /// three in the afternoon; the other half of `mAlpha` hides a moon under
        /// `Moons_<name>_Fade_End_Angle`, which is the engine keeping a lit quad off its own fogged
        /// horizon. A renderer with air in it wants the first without the second.
        float mDaylightFade = 0.0f;
    };

    /// One moon's clock, out of the `Moons_<name>_*` settings.
    ///
    /// **Reverse engineered from Morrowind's own scene graph**, and every odd-looking constant in
    /// the implementation carries the reason it is that number. It is a component rather than game
    /// code because it is arithmetic over settings and nothing else: `MWWorld::MoonModel` wraps it
    /// for the weather system, and anything else holding a day and an hour can ask it directly. Two
    /// copies of this arithmetic is what there used to be, and the only thing keeping them in step
    /// was a test.
    ///
    /// Nothing here reads the world. Given a day and an hour it is pure arithmetic over the
    /// settings, which is what lets it sit below whatever asks.
    class MoonModel
    {
    public:
        /// Reads `Moons_<name>_*` — "Masser" or "Secunda", as the settings spell them.
        explicit MoonModel(std::string_view name);

        MoonModel(float fadeInStart, float fadeInFinish, float fadeOutStart, float fadeOutFinish, float axisOffset,
            float speed, float dailyIncrement, float fadeStartAngle, float fadeEndAngle,
            float moonShadowEarlyFadeAngle);

        /// @param day days since the world began, on Morrowind's own count: a new game starts at
        ///        zero, which the rise-hour formula anchors to 16 Last Seed.
        /// @param hour on a twenty-four hour clock.
        MoonMoment at(int day, float hour) const;

    private:
        float mFadeInStart;
        float mFadeInFinish;
        float mFadeOutStart;
        float mFadeOutFinish;
        float mAxisOffset;
        float mSpeed;
        float mDailyIncrement;
        float mFadeStartAngle;
        float mFadeEndAngle;
        float mEarlyFadeAngle;

        /// Degrees travelled in `hours`. Fifteen an hour is one rotation a day, so the speed counts
        /// whole rotations a day rather than being a rate.
        float rotation(float hours) const;

        float riseHour(int day) const;
        float angle(int day, float hour) const;
        float hourlyAlpha(float hour) const;
        float earlyShadowAlpha(float angle) const;
        float shadowBlend(float angle) const;
        bool isVisible(int day, float hour) const;
        float phaseHour(int day) const;
        MoonPhase phase(int day, float hour) const;
    };
}
