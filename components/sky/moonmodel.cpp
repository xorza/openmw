#include "moonmodel.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <components/fallback/fallback.hpp>

namespace Sky
{
    namespace
    {
        float setting(std::string_view name, std::string_view field)
        {
            return Fallback::Map::getFloat("Moons_" + std::string(name) + "_" + std::string(field));
        }

        /// A moon slower than this could not finish its arc in a day, so the engine floors every one
        /// of them here: half a hemisphere over twenty-three hours, at fifteen degrees an hour.
        constexpr float sSlowest = 180.0f / 23.0f / 15.0f;
    }

    MoonModel::MoonModel(std::string_view name)
        : MoonModel(setting(name, "Fade_In_Start"), setting(name, "Fade_In_Finish"), setting(name, "Fade_Out_Start"),
            setting(name, "Fade_Out_Finish"), setting(name, "Axis_Offset"), setting(name, "Speed"),
            setting(name, "Daily_Increment"), setting(name, "Fade_Start_Angle"), setting(name, "Fade_End_Angle"),
            setting(name, "Moon_Shadow_Early_Fade_Angle"))
    {
    }

    MoonModel::MoonModel(float fadeInStart, float fadeInFinish, float fadeOutStart, float fadeOutFinish,
        float axisOffset, float speed, float dailyIncrement, float fadeStartAngle, float fadeEndAngle,
        float moonShadowEarlyFadeAngle)
        : mFadeInStart(fadeInStart)
        , mFadeInFinish(fadeInFinish)
        , mFadeOutStart(fadeOutStart)
        , mFadeOutFinish(fadeOutFinish)
        , mAxisOffset(axisOffset)
        , mSpeed(std::max(speed, sSlowest))

        // Reduced modulo a day for the reason the speed has a floor: an increment past twenty-four
        // would advance the moon more than a whole rotation between one day and the next.
        , mDailyIncrement(std::fmod(dailyIncrement, 24.0f))
        , mFadeStartAngle(fadeStartAngle)
        , mFadeEndAngle(fadeEndAngle)
        , mEarlyFadeAngle(moonShadowEarlyFadeAngle)
    {
    }

    float MoonModel::rotation(float hours) const
    {
        return 15.0f * mSpeed * hours;
    }

    float MoonModel::riseHour(int day) const
    {
        if (mDailyIncrement == 0.0f)
            return 0.0f;

        // 16 Last Seed, where Morrowind's own count is anchored: seventeen increments have already
        // happened by the time a new game begins.
        constexpr int startDay = 16;

        // What makes the rise hour a twenty-four day loop: the increments the engine skips,
        // multiplied by however many loops have gone by.
        const float missed
            = (24.0f - std::abs(24.0f / mDailyIncrement)) * std::floor(static_cast<float>(day + startDay) / 24.0f);

        // No modulo after the last increment: a rise hour at or past twenty-four is how the engine
        // says "not until tomorrow", and `angle` asks that question.
        return mDailyIncrement + std::fmod((static_cast<float>(day - 1 + startDay) - missed) * mDailyIncrement, 24.0f);
    }

    float MoonModel::angle(int day, float hour) const
    {
        const float riseToday = riseHour(day);
        float travelled = 0.0f;

        if (hour < riseToday)
        {
            // It may still be up from yesterday, whose rise hour is one increment earlier.
            const float riseYesterday = riseToday - mDailyIncrement;
            if (riseYesterday < 24.0f)
            {
                // The engine offsets the increment by one where yesterday's visible point crossed
                // into today, and holds that offset until the loop restarts.
                const float toVisible = (mFadeEndAngle - mEarlyFadeAngle) / rotation(1.0f);
                const float offset = riseYesterday + toVisible > 24.0f ? mDailyIncrement : 0.0f;

                const float yesterday = rotation(24.0f - (riseYesterday + offset));
                if (yesterday < 180.0f)
                    travelled = rotation(hour) + yesterday;
            }
        }
        else
        {
            travelled = rotation(hour - riseToday);
        }

        // Past the far horizon it has set, and the engine resets the angle rather than letting it
        // run on.
        return travelled >= 180.0f ? 0.0f : travelled;
    }

    float MoonModel::hourlyAlpha(float hour) const
    {
        // The engine culls the moon one minute before the fade finishes, which is why the finish is
        // pulled back by one.
        constexpr float oneMinute = 0.0167f;
        const float fadeOutFinish = mFadeOutFinish - oneMinute;

        if (hour >= mFadeOutStart && hour < fadeOutFinish)
            return (fadeOutFinish - hour) / (fadeOutFinish - mFadeOutStart);
        if (hour >= fadeOutFinish && hour < mFadeInStart)
            return 0.0f;
        if (hour >= mFadeInStart && hour < mFadeInFinish)
            return (hour - mFadeInStart) / (mFadeInFinish - mFadeInStart);

        return 1.0f;
    }

    float MoonModel::earlyShadowAlpha(float angle) const
    {
        // An arc either side of the fade end angles, over which the moon arrives and leaves.
        const float rising = mFadeEndAngle - mEarlyFadeAngle;
        const float endSetting = 180.0f - mFadeEndAngle;
        const float setting = endSetting + mEarlyFadeAngle;

        if (angle >= rising && angle < mFadeEndAngle)
            return (angle - rising) / mEarlyFadeAngle;
        if (angle >= mFadeEndAngle && angle < endSetting)
            return 1.0f;
        if (angle >= endSetting && angle < setting)
            return (setting - angle) / mEarlyFadeAngle;

        return 0.0f;
    }

    float MoonModel::shadowBlend(float angle) const
    {
        // Between the two fade angles the moon crossfades from a solid disc roughly the colour of
        // the sky to its textured face, and back again as it sets.
        const float across = mFadeStartAngle - mFadeEndAngle;
        const float endSetting = 180.0f - mFadeEndAngle;
        const float startSetting = 180.0f - mFadeStartAngle;

        if (angle >= mFadeEndAngle && angle < mFadeStartAngle)
            return (angle - mFadeEndAngle) / across;
        if (angle >= mFadeStartAngle && angle < startSetting)
            return 1.0f;
        if (angle >= startSetting && angle < endSetting)
            return (endSetting - angle) / across;

        return 0.0f;
    }

    bool MoonModel::isVisible(int day, float hour) const
    {
        return hourlyAlpha(hour) > 0.0f && earlyShadowAlpha(angle(day, hour)) > 0.0f;
    }

    float MoonModel::phaseHour(int day) const
    {
        // Morrowind holds a phase change back until the moon is invisible — at midnight, or one
        // increment past the angle where it has already faded.
        if (!isVisible(day, 0.0f))
            return 0.0f;

        const float faded = (180.0f - mFadeEndAngle) + mEarlyFadeAngle;
        return ((faded - angle(day, 0.0f)) / rotation(1.0f)) + std::max(mDailyIncrement, 0.0f);
    }

    MoonPhase MoonModel::phase(int day, float hour) const
    {
        // Full on 16 Last Seed, waning from the seventeenth on a three-day cycle — and a moon that
        // has not risen yet today is still showing yesterday's.
        const int counted = hour < phaseHour(day) ? day : day + 1;
        return static_cast<MoonPhase>((counted / 3) % 8);
    }

    MoonMoment MoonModel::at(int day, float hour) const
    {
        const float along = angle(day, hour);

        return MoonMoment{
            .mAlongArc = along,
            .mAxisOffset = mAxisOffset,
            .mPhase = phase(day, hour),
            .mShadowBlend = shadowBlend(along),
            .mAlpha = earlyShadowAlpha(along) * hourlyAlpha(hour),
        };
    }
}
