#include "weather.hpp"

#include <components/esm/stringrefid.hpp>
#include <components/settings/values.hpp>

#include <components/misc/rng.hpp>
#include <components/sky/skyclock.hpp>
#include <components/sky/sundisc.hpp>

#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm3/weatherstate.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwsound/sound.hpp"

#include "../mwrender/renderingmanager.hpp"

#include "cellstore.hpp"
#include "datetimemanager.hpp"
#include "esmstore.hpp"
#include "player.hpp"

#include <algorithm>
#include <cmath>

namespace MWWorld
{
    namespace
    {
        // linear interpolate between x and y based on factor.
        float lerp(float x, float y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }
        // linear interpolate between x and y based on factor.
        osg::Vec4f lerp(const osg::Vec4f& x, const osg::Vec4f& y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }

        osg::Vec3f calculateStormDirection(const std::string& particleEffect)
        {
            osg::Vec3f stormDirection = MWWorld::Weather::defaultDirection();
            if (particleEffect == Settings::models().mWeatherashcloud.get()
                || particleEffect == Settings::models().mWeatherblightcloud.get())
            {
                osg::Vec3f playerPos = MWMechanics::getPlayer().getRefData().getPosition().asVec3();
                playerPos.z() = 0;
                osg::Vec3f redMountainPos = osg::Vec3f(25000.f, 70000.f, 0.f);
                stormDirection = (playerPos - redMountainPos);
                stormDirection.normalize();
            }
            return stormDirection;
        }
    }

    osg::Vec3f Weather::defaultDirection()
    {
        static const osg::Vec3f direction = osg::Vec3f(0.f, 1.f, 0.f);
        return direction;
    }

    Weather::Weather(ESM::RefId id, int scriptId, const std::string& name, float stormWindSpeed, float dlFactor,
        float dlOffset, std::string_view particleEffect)
        : mId(id)
        , mScriptId(scriptId)
        , mName(name)
        , mCloudTexture(Fallback::Map::getString("Weather_" + name + "_Cloud_Texture"))
        , mSkyColor(Fallback::Map::getColour("Weather_" + name + "_Sky_Sunrise_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sky_Day_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sky_Sunset_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sky_Night_Color"))
        , mFogColor(Fallback::Map::getColour("Weather_" + name + "_Fog_Sunrise_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Fog_Day_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Fog_Sunset_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Fog_Night_Color"))
        , mAmbientColor(Fallback::Map::getColour("Weather_" + name + "_Ambient_Sunrise_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Ambient_Day_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Ambient_Sunset_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Ambient_Night_Color"))
        , mSunColor(Fallback::Map::getColour("Weather_" + name + "_Sun_Sunrise_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sun_Day_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sun_Sunset_Color"),
              Fallback::Map::getColour("Weather_" + name + "_Sun_Night_Color"))
        , mLandFogDepth(Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Day_Depth"),
              Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Day_Depth"),
              Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Day_Depth"),
              Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Night_Depth"))
        , mSunDiscSunsetColor(Fallback::Map::getColour("Weather_" + name + "_Sun_Disc_Sunset_Color"))
        , mWindSpeed(Fallback::Map::getFloat("Weather_" + name + "_Wind_Speed"))
        , mCloudSpeed(Fallback::Map::getFloat("Weather_" + name + "_Cloud_Speed"))
        , mGlareView(Fallback::Map::getFloat("Weather_" + name + "_Glare_View"))
        , mIsStorm(mWindSpeed > stormWindSpeed)
        , mRainSpeed(Fallback::Map::getFloat("Weather_Precip_Gravity"))
        , mRainEntranceSpeed(Fallback::Map::getFloat("Weather_" + name + "_Rain_Entrance_Speed"))
        , mRainMaxRaindrops(Fallback::Map::getInt("Weather_" + name + "_Max_Raindrops"))
        , mRainDiameter(Fallback::Map::getFloat("Weather_" + name + "_Rain_Diameter"))
        , mRainThreshold(Fallback::Map::getFloat("Weather_" + name + "_Rain_Threshold"))
        , mRainMinHeight(Fallback::Map::getFloat("Weather_" + name + "_Rain_Height_Min"))
        , mRainMaxHeight(Fallback::Map::getFloat("Weather_" + name + "_Rain_Height_Max"))
        , mParticleEffect(particleEffect)
        , mRainEffect(Fallback::Map::getBool("Weather_" + name + "_Using_Precip") ? "meshes\\raindrop.nif" : "")
        , mStormDirection(Weather::defaultDirection())
        , mCloudsMaximumPercent(Fallback::Map::getFloat("Weather_" + name + "_Clouds_Maximum_Percent"))
        , mTransitionDelta(Fallback::Map::getFloat("Weather_" + name + "_Transition_Delta"))
        , mThunderFrequency(Fallback::Map::getFloat("Weather_" + name + "_Thunder_Frequency"))
        , mThunderThreshold(Fallback::Map::getFloat("Weather_" + name + "_Thunder_Threshold"))
        , mFlashDecrement(Fallback::Map::getFloat("Weather_" + name + "_Flash_Decrement"))
        , mFlashBrightness(0.0f)
    {
        mDL.FogFactor = dlFactor;
        mDL.FogOffset = dlOffset;
        mThunderSoundID[0]
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Thunder_Sound_ID_0"));
        mThunderSoundID[1]
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Thunder_Sound_ID_1"));
        mThunderSoundID[2]
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Thunder_Sound_ID_2"));
        mThunderSoundID[3]
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Thunder_Sound_ID_3"));

        if (!mRainEffect.empty()) // NOTE: in vanilla, the weathers with rain seem to be hardcoded; changing
                                  // Using_Precip has no effect
        {
            mRainLoopSoundID
                = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Rain_Loop_Sound_ID"));
            if (mRainLoopSoundID.empty()) // default to "rain" if not set
                mRainLoopSoundID = ESM::RefId::stringRefId("rain");
            else if (mRainLoopSoundID == "None")
                mRainLoopSoundID = ESM::RefId();
        }

        mAmbientLoopSoundID
            = ESM::RefId::stringRefId(Fallback::Map::getString("Weather_" + name + "_Ambient_Loop_Sound_ID"));
        if (mAmbientLoopSoundID == "None")
            mAmbientLoopSoundID = ESM::RefId();
    }

    float Weather::transitionDelta() const
    {
        // Transition Delta describes how quickly transitioning to the weather in question will take, in Hz. Note that
        // the measurement is in real time, not in-game time.
        return mTransitionDelta;
    }

    float Weather::cloudBlendFactor(const float transitionRatio) const
    {
        // Clouds Maximum Percent affects how quickly the sky transitions from one sky texture to the next.
        return transitionRatio / mCloudsMaximumPercent;
    }

    float Weather::calculateThunder(const float transitionRatio, const float elapsedSeconds, const bool isPaused)
    {
        // When paused, the flash brightness remains the same and no new strikes can occur.
        if (!isPaused)
        {
            // Morrowind doesn't appear to do any calculations unless the transition ratio is higher than the Thunder
            // Threshold.
            if (transitionRatio >= mThunderThreshold && mThunderFrequency > 0.0f)
            {
                flashDecrement(elapsedSeconds);
                auto& prng = MWBase::Environment::get().getWorld()->getPrng();
                if (Misc::Rng::rollProbability(prng) <= thunderChance(transitionRatio, elapsedSeconds))
                {
                    lightningAndThunder();
                }
            }
            else
            {
                mFlashBrightness = 0.0f;
            }
        }

        return mFlashBrightness;
    }

    inline void Weather::flashDecrement(const float elapsedSeconds)
    {
        // The Flash Decrement is measured in whole units per second. This means that if the flash brightness was
        // currently 1.0, then it should take approximately 0.25 seconds to decay to 0.0 (the minimum).
        float decrement = mFlashDecrement * elapsedSeconds;
        mFlashBrightness = decrement > mFlashBrightness ? 0.0f : mFlashBrightness - decrement;
    }

    inline float Weather::thunderChance(const float transitionRatio, const float elapsedSeconds) const
    {
        // This formula is reversed from the observation that with Thunder Frequency set to 1, there are roughly 10
        // strikes per minute. It doesn't appear to be tied to in game time as Timescale doesn't affect it. Various
        // values of Thunder Frequency seem to change the average number of strikes in a linear fashion.. During a
        // transition, it appears to scaled based on how far past it is past the Thunder Threshold.
        float scaleFactor = (transitionRatio - mThunderThreshold) / (1.0f - mThunderThreshold);
        return ((mThunderFrequency * 10.0f) / 60.0f) * elapsedSeconds * scaleFactor;
    }

    inline void Weather::lightningAndThunder(void)
    {
        // Morrowind seems to vary the intensity of the brightness based on which of the four sound IDs it selects.
        // They appear to go from 0 (brightest, closest) to 3 (faintest, farthest). The value of 0.25 per distance
        // was derived by setting the Flash Decrement to 0.1 and measuring how long each value took to decay to 0.
        // TODO: Determine the distribution of each distance to see if it's evenly weighted.
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        unsigned int distance = Misc::Rng::rollDice(4, prng);
        // Flash brightness appears additive, since if multiple strikes occur, it takes longer for it to decay to 0.
        mFlashBrightness += 1 - (distance * 0.25f);
        MWBase::Environment::get().getSoundManager()->playSound(mThunderSoundID[distance], 1.0, 1.0);
    }

    RegionWeather::RegionWeather(const ESM::Region& region)
        : mChances(region.mData.mProbabilities)
    {
    }

    RegionWeather::RegionWeather(const ESM::RegionWeatherState& state)
        : mWeather(state.mWeather)
        , mChances(state.mChances)
    {
    }

    RegionWeather::operator ESM::RegionWeatherState() const
    {
        ESM::RegionWeatherState state = { mWeather, mChances };

        return state;
    }

    void RegionWeather::setChances(const std::map<ESM::RefId, uint8_t>& chances, const WeatherStore& store)
    {
        mChances = chances;

        // Regional weather no longer supports the current type, select a new weather pattern.
        if (getChance(mWeather) == 0)
        {
            chooseNewWeather(store);
        }
    }

    uint8_t RegionWeather::getChance(ESM::RefId weather) const
    {
        const auto found = mChances.find(weather);
        if (found != mChances.end())
            return found->second;
        return 0;
    }

    void RegionWeather::setWeather(ESM::RefId weatherID)
    {
        mWeather = weatherID;
    }

    ESM::RefId RegionWeather::getWeather(const WeatherStore& store)
    {
        // If the region weather was already set (by ChangeWeather, or by a previous call) then just return that value.
        // Note that the region weather will be expired periodically when the weather update timer expires.
        if (mWeather.empty())
        {
            chooseNewWeather(store);
        }

        return mWeather;
    }

    void RegionWeather::chooseNewWeather(const WeatherStore& store)
    {
        // All probabilities must add to 100 (responsibility of the user).
        // If chances A and B has values 30 and 70 then by generating 100 numbers 1..100, 30% will be lesser or equal 30
        // and 70% will be greater than 30 (in theory).
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        unsigned int chance = Misc::Rng::rollDice(100u, prng) + 1u; // 1..100
        unsigned int sum = 0;
        for (const Weather* weather : store)
        {
            sum += getChance(weather->mId);
            if (chance <= sum)
            {
                mWeather = weather->mId;
                return;
            }
        }

        // if we hit this path then the chances don't add to 100, choose a default weather instead
        mWeather = ESM::Weather::indexToRefId(0);
    }

    MoonModel::MoonModel(float fadeInStart, float fadeInFinish, float fadeOutStart, float fadeOutFinish,
        float axisOffset, float speed, float dailyIncrement, float fadeStartAngle, float fadeEndAngle,
        float moonShadowEarlyFadeAngle)
    {
        mFadeInStart = fadeInStart;
        mFadeInFinish = fadeInFinish;
        mFadeOutStart = fadeOutStart;
        mFadeOutFinish = fadeOutFinish;
        mAxisOffset = axisOffset;
        mDailyIncrement = dailyIncrement;
        mFadeStartAngle = fadeStartAngle;
        mFadeEndAngle = fadeEndAngle;
        mMoonShadowEarlyFadeAngle = moonShadowEarlyFadeAngle;

        // Morrowind appears to have a minimum speed to avoid situations where the moon can't
        // complete a full rotation in a single 24-hour period. The reverse-engineered formula is
        // 180 degrees (full hemisphere) / 23 hours / 15 degrees (1 hour travel at speed 1.0).
        mSpeed = std::max(speed, (180.0f / 23.0f / 15.0f));

        // Morrowind appears to reduce mDailyIncrement with modulo 24.0f to avoid situations where
        // the moon would increment more than an entire rotation in a single day.
        mDailyIncrement = std::fmod(mDailyIncrement, 24.0f);
    }

    MoonModel::MoonModel(const std::string& name)
        : mFadeInStart(Fallback::Map::getFloat("Moons_" + name + "_Fade_In_Start"))
        , mFadeInFinish(Fallback::Map::getFloat("Moons_" + name + "_Fade_In_Finish"))
        , mFadeOutStart(Fallback::Map::getFloat("Moons_" + name + "_Fade_Out_Start"))
        , mFadeOutFinish(Fallback::Map::getFloat("Moons_" + name + "_Fade_Out_Finish"))
        , mAxisOffset(Fallback::Map::getFloat("Moons_" + name + "_Axis_Offset"))
        , mSpeed(Fallback::Map::getFloat("Moons_" + name + "_Speed"))
        , mDailyIncrement(Fallback::Map::getFloat("Moons_" + name + "_Daily_Increment"))
        , mFadeStartAngle(Fallback::Map::getFloat("Moons_" + name + "_Fade_Start_Angle"))
        , mFadeEndAngle(Fallback::Map::getFloat("Moons_" + name + "_Fade_End_Angle"))
        , mMoonShadowEarlyFadeAngle(Fallback::Map::getFloat("Moons_" + name + "_Moon_Shadow_Early_Fade_Angle"))
    {
        // Morrowind appears to have a minimum speed to avoid situations where the moon can't
        // complete a full rotation in a single 24-hour period. The reverse-engineered formula is
        // 180 degrees (full hemisphere) / 23 hours / 15 degrees (1 hour travel at speed 1.0).
        mSpeed = std::max(mSpeed, (180.0f / 23.0f / 15.0f));

        // Morrowind appears to reduce mDailyIncrement with modulo 24.0f to avoid situations where
        // the moon would increment more than an entire rotation in a single day.
        mDailyIncrement = std::fmod(mDailyIncrement, 24.0f);
    }

    MWRender::MoonState MoonModel::calculateState(const TimeStamp& gameTime) const
    {
        float rotationFromHorizon = angle(gameTime.getDay(), gameTime.getHour());
        const float daylightFade = hourlyAlpha(gameTime.getHour());
        MWRender::MoonState state = { rotationFromHorizon,
            mAxisOffset, // Reverse engineered from Morrowind's scene graph rotation matrices.
            phase(gameTime), shadowBlend(rotationFromHorizon), earlyMoonShadowAlpha(rotationFromHorizon) * daylightFade,
            daylightFade };

        return state;
    }

    inline float MoonModel::angle(int gameDay, float gameHour) const
    {
        // Morrowind's moons start travel on one side of the horizon (let's call it H-rise) and travel 180 degrees to
        // the opposite horizon (let's call it H-set). Upon reaching H-set, they reset to H-rise until the next moon
        // rise.

        // When calculating the angle of the moon, several cases have to be taken into account:
        // 1. Moon rises and then sets in one day.
        // 2. Moon sets and doesn't rise in one day (occurs when the moon rise hour is >= 24).
        // 3. Moon sets and then rises in one day.
        float moonRiseHourToday = moonRiseHour(gameDay);
        float moonRiseAngleToday = 0.0f;

        if (gameHour < moonRiseHourToday)
        {
            // Rise hour increases by mDailyIncrement each day, so yesterday's is easy to calculate
            float moonRiseHourYesterday = moonRiseHourToday - mDailyIncrement;
            if (moonRiseHourYesterday < 24.0f)
            {
                // Morrowind offsets the increment by -1 when the previous day's visible point crosses into the next
                // day. The offset lasts from this point until the next 24-day loop starts. To find this point we add
                // mDailyIncrement to the previous visible point and check the result.
                float moonShadowEarlyFadeAngle1 = mFadeEndAngle - mMoonShadowEarlyFadeAngle;
                float timeToVisible = moonShadowEarlyFadeAngle1 / rotation(1.0f);
                float cycleOffset = moonRiseHourYesterday + timeToVisible > 24.0f ? mDailyIncrement : 0.0f;

                float moonRiseAngleYesterday = rotation(24.0f - (moonRiseHourYesterday + cycleOffset));
                if (moonRiseAngleYesterday < 180.0f)
                {
                    // The moon rose but did not set yesterday, so accumulate yesterday's angle with how much we've
                    // travelled today.
                    moonRiseAngleToday = rotation(gameHour) + moonRiseAngleYesterday;
                }
            }
        }
        else
        {
            moonRiseAngleToday = rotation(gameHour - moonRiseHourToday);
        }

        if (moonRiseAngleToday >= 180.0f)
        {
            // The moon set today, reset the angle to the horizon.
            moonRiseAngleToday = 0.0f;
        }

        return moonRiseAngleToday;
    }

    inline float MoonModel::moonPhaseHour(int gameDay) const
    {
        // Morrowind delays moon phase changes until one of these is true:
        //   * The moon is invisible at midnight.
        //   * The moon reached moonShadowEarlyFadeAngle2 one daily increment ago (therefore invisible).
        if (!isVisible(gameDay, 0.0f))
            return 0.0f;
        else
        {
            // Calculate the angle at which the moon becomes transparent and the starting angle.
            float moonShadowEarlyFadeAngle2 = (180.0f - mFadeEndAngle) + mMoonShadowEarlyFadeAngle;
            float midnightAngle = angle(gameDay, 0.0f);

            // We can assume that moonShadowEarlyFadeAngle2 > midnightAngle, because the opposite
            // case would make the moon invisible at midnight, which is checked above.
            return ((moonShadowEarlyFadeAngle2 - midnightAngle) / rotation(1.0f)) + std::max(mDailyIncrement, 0.0f);
        }
    }

    inline float MoonModel::moonRiseHour(int gameDay) const
    {
        if (mDailyIncrement == 0.0f)
            return 0.0f;

        // This arises from the start date of 16 Last Seed, 427
        // TODO: Find an alternate formula that doesn't rely on this day being fixed.
        constexpr int startDay = 16;

        // This formula finds the number of missed increments necessary to make the rise hour a 24-day loop.
        // The offset increases on the first day of the loop and is multiplied by the number of completed loops.
        float incrementOffset = (24.0f - std::abs(24.0f / mDailyIncrement)) * std::floor((gameDay + startDay) / 24.0f);

        // This odd formula arises from the fact that on 16 Last Seed, 17 increments have occurred, meaning
        // that upon starting a new game, it must only calculate the moon phase as far back as 1 Last Seed.
        // Note that we don't modulo after adding the latest daily increment because other calculations need to
        // know if doing so would cause the moon rise to be postponed until the next day (which happens when
        // the moon rise hour is >= 24 in Morrowind).
        return mDailyIncrement + std::fmod((gameDay - 1 + startDay - incrementOffset) * mDailyIncrement, 24.0f);
    }

    inline float MoonModel::rotation(float hours) const
    {
        // 15 degrees per hour was reverse engineered from the rotation matrices of the Morrowind scene graph.
        // Note that this correlates to 360 / 24, which is a full rotation every 24 hours, so speed is a measure
        // of whole rotations that could be completed in a day.
        return 15.0f * mSpeed * hours;
    }

    MWRender::MoonState::Phase MoonModel::phase(const TimeStamp& gameTime) const
    {
        // Morrowind starts with a full moon on 16 Last Seed and then begins to wane 17 Last Seed, working on 3 day
        // phase cycle.

        // If the moon didn't rise yet today, use yesterday's moon phase.
        if (gameTime.getHour() < moonPhaseHour(gameTime.getDay()))
            return static_cast<MWRender::MoonState::Phase>((gameTime.getDay() / 3) % 8);
        else
            return static_cast<MWRender::MoonState::Phase>(((gameTime.getDay() + 1) / 3) % 8);
    }

    inline bool MoonModel::isVisible(int gameDay, float gameHour) const
    {
        // Moons are "visible" when their alpha value is non-zero.
        return hourlyAlpha(gameHour) > 0.f && earlyMoonShadowAlpha(angle(gameDay, gameHour)) > 0.f;
    }

    inline float MoonModel::shadowBlend(float angle) const
    {
        // The Fade End Angle and Fade Start Angle describe a region where the moon transitions from a solid disk
        // that is roughly the color of the sky, to a textured surface.
        // Depending on the current angle, the following values describe the ratio between the textured moon
        // and the solid disk:
        // 1. From Fade End Angle 1 to Fade Start Angle 1 (during moon rise): 0..1
        // 2. From Fade Start Angle 1 to Fade Start Angle 2 (between moon rise and moon set): 1 (textured)
        // 3. From Fade Start Angle 2 to Fade End Angle 2 (during moon set): 1..0
        // 4. From Fade End Angle 2 to Fade End Angle 1 (between moon set and moon rise): 0 (solid disk)
        float fadeAngle = mFadeStartAngle - mFadeEndAngle;
        float fadeEndAngle2 = 180.0f - mFadeEndAngle;
        float fadeStartAngle2 = 180.0f - mFadeStartAngle;
        if ((angle >= mFadeEndAngle) && (angle < mFadeStartAngle))
            return (angle - mFadeEndAngle) / fadeAngle;
        else if ((angle >= mFadeStartAngle) && (angle < fadeStartAngle2))
            return 1.0f;
        else if ((angle >= fadeStartAngle2) && (angle < fadeEndAngle2))
            return (fadeEndAngle2 - angle) / fadeAngle;
        else
            return 0.0f;
    }

    inline float MoonModel::hourlyAlpha(float gameHour) const
    {
        // Morrowind culls the moon one minute before mFadeOutFinish
        constexpr float oneMinute = 0.0167f;
        float adjustedFadeOutFinish = mFadeOutFinish - oneMinute;

        // The Fade Out Start / Finish and Fade In Start / Finish describe the hours at which the moon
        // appears and disappears.
        // Depending on the current hour, the following values describe how transparent the moon is.
        // 1. From Fade Out Start to Fade Out Finish: 1..0
        // 2. From Fade Out Finish to Fade In Start: 0 (transparent)
        // 3. From Fade In Start to Fade In Finish: 0..1
        // 4. From Fade In Finish to Fade Out Start: 1 (solid)
        if ((gameHour >= mFadeOutStart) && (gameHour < adjustedFadeOutFinish))
            return (adjustedFadeOutFinish - gameHour) / (adjustedFadeOutFinish - mFadeOutStart);
        else if ((gameHour >= adjustedFadeOutFinish) && (gameHour < mFadeInStart))
            return 0.0f;
        else if ((gameHour >= mFadeInStart) && (gameHour < mFadeInFinish))
            return (gameHour - mFadeInStart) / (mFadeInFinish - mFadeInStart);
        else
            return 1.0f;
    }

    inline float MoonModel::earlyMoonShadowAlpha(float angle) const
    {
        // The Moon Shadow Early Fade Angle describes an arc relative to Fade End Angle.
        // Depending on the current angle, the following values describe how transparent the moon is.
        // 1. From Moon Shadow Early Fade Angle 1 to Fade End Angle 1 (during moon rise): 0..1
        // 2. From Fade End Angle 1 to Fade End Angle 2 (between moon rise and moon set): 1 (solid)
        // 3. From Fade End Angle 2 to Moon Shadow Early Fade Angle 2 (during moon set): 1..0
        // 4. From Moon Shadow Early Fade Angle 2 to Moon Shadow Early Fade Angle 1: 0 (transparent)
        float moonShadowEarlyFadeAngle1 = mFadeEndAngle - mMoonShadowEarlyFadeAngle;
        float fadeEndAngle2 = 180.0f - mFadeEndAngle;
        float moonShadowEarlyFadeAngle2 = fadeEndAngle2 + mMoonShadowEarlyFadeAngle;
        if ((angle >= moonShadowEarlyFadeAngle1) && (angle < mFadeEndAngle))
            return (angle - moonShadowEarlyFadeAngle1) / mMoonShadowEarlyFadeAngle;
        else if ((angle >= mFadeEndAngle) && (angle < fadeEndAngle2))
            return 1.0f;
        else if ((angle >= fadeEndAngle2) && (angle < moonShadowEarlyFadeAngle2))
            return (moonShadowEarlyFadeAngle2 - angle) / mMoonShadowEarlyFadeAngle;
        else
            return 0.0f;
    }

    void WeatherStore::reset(const MWWorld::ESMStore& store)
    {
        mShared.clear();
        mStatic.clear();
        static const float fStromWindSpeed = store.get<ESM::GameSetting>().find("fStromWindSpeed")->mValue.getFloat();
        const auto addWeather
            = [&](const std::string& name, float dlFactor, float dlOffset, std::string_view particleEffect = {}) {
                  const int index = static_cast<int>(getSize());
                  Weather weather(ESM::Weather::indexToRefId(index), index, name, fStromWindSpeed, dlFactor, dlOffset,
                      particleEffect);

                  insertStatic(std::move(weather));
              };
        // These distant land fog factor and offset values are the defaults MGE XE provides. Should be
        // provided by settings somewhere?
        addWeather("Clear", 1.0f, 0.0f); // 0
        addWeather("Cloudy", 0.9f, 0.0f); // 1
        addWeather("Foggy", 0.2f, 30.0f); // 2
        addWeather("Overcast", 0.7f, 0.0f); // 3
        addWeather("Rain", 0.5f, 10.0f); // 4
        addWeather("Thunderstorm", 0.5f, 20.0f); // 5
        addWeather("Ashstorm", 0.2f, 50.0f, Settings::models().mWeatherashcloud.get()); // 6
        addWeather("Blight", 0.2f, 60.0f, Settings::models().mWeatherblightcloud.get()); // 7
        addWeather("Snow", 0.5f, 40.0f, Settings::models().mWeathersnow.get()); // 8
        addWeather("Blizzard", 0.16f, 70.0f, Settings::models().mWeatherblizzard.get()); // 9
    }

    const Weather* WeatherStore::search(ESM::RefId id) const
    {
        const auto it = mStatic.find(id);
        if (it == mStatic.end())
            return nullptr;
        return &it->second;
    }

    const Weather* WeatherStore::find(ESM::RefId id) const
    {
        if (const Weather* weather = search(id))
            return weather;
        throw std::runtime_error("Weather " + id.toDebugString() + " not found");
    }

    Weather* WeatherStore::find(ESM::RefId id)
    {
        const auto* self = this;
        return const_cast<Weather*>(self->find(id));
    }

    Weather* WeatherStore::insertStatic(Weather&& item)
    {
        const auto [it, inserted] = mStatic.insert_or_assign(item.mId, std::move(item));
        Weather* ptr = &it->second;
        if (inserted)
            mShared.push_back(ptr);
        return ptr;
    }

    void WeatherStore::eraseStatic(ESM::RefId id)
    {
        const auto found = mStatic.find(id);
        if (found == mStatic.end())
            return;
        for (auto it = mShared.begin(); it != mShared.end(); ++it)
        {
            if ((*it)->mId == id)
            {
                mShared.erase(it);
                break;
            }
        }
        mStatic.erase(found);
    }

    std::vector<Moon> WeatherManager::getCurrentMoons(const TimeStamp& time) const
    {
        const auto makeMoon = [](std::string_view name, const MoonModel& model, const TimeStamp& timestamp) {
            const MWRender::MoonState state = model.calculateState(timestamp);
            return Moon{ name, state.mPhase, MWRender::MoonState::phaseToInt(state.mPhase), state.mMoonAlpha };
        };

        return { makeMoon("Masser", mMasser, time), makeMoon("Secunda", mSecunda, time) };
    }

    WeatherManager::WeatherManager(
        MWRender::RenderingManager& rendering, MWWorld::ESMStore& store, MWWorld::WeatherStore& weatherStore)
        : mStore(store)
        , mRendering(rendering)
        , mSunriseTime(Fallback::Map::getFloat("Weather_Sunrise_Time"))
        , mSunsetTime(Fallback::Map::getFloat("Weather_Sunset_Time"))
        , mSunriseDuration(Fallback::Map::getFloat("Weather_Sunrise_Duration"))
        , mSunsetDuration(Fallback::Map::getFloat("Weather_Sunset_Duration"))
        , mSunPreSunsetTime(Fallback::Map::getFloat("Weather_Sun_Pre-Sunset_Time"))
        , mNightFade(0, 0, 0, 1)
        , mHoursBetweenWeatherChanges(Fallback::Map::getFloat("Weather_Hours_Between_Weather_Changes"))
        , mUnderwaterFog(Fallback::Map::getFloat("Water_UnderwaterSunriseFog"),
              Fallback::Map::getFloat("Water_UnderwaterDayFog"), Fallback::Map::getFloat("Water_UnderwaterSunsetFog"),
              Fallback::Map::getFloat("Water_UnderwaterNightFog"))
        , mWeatherStore(&weatherStore)
        , mMasser("Masser")
        , mSecunda("Secunda")
        , mWindSpeed(0.f)
        , mCurrentWindSpeed(0.f)
        , mNextWindSpeed(0.f)
        , mIsStorm(false)
        , mPrecipitation(false)
        , mStormDirection(Weather::defaultDirection())
        , mCurrentRegion()
        , mTimePassed(0)
        , mFastForward(false)
        , mWeatherUpdateTime(mHoursBetweenWeatherChanges)
        , mTransitionFactor(0)
        , mNightDayMode(Default)
        , mRegions()
    {
        mSky.mTimes = Sky::TimeOfDaySettings::fromFallback();

        mWeatherStore->reset(mStore);

        Store<ESM::Region>::iterator it = store.get<ESM::Region>().begin();
        for (; it != store.get<ESM::Region>().end(); ++it)
        {
            mRegions.insert(std::make_pair(it->mId, RegionWeather(*it)));
        }

        forceWeather(ESM::Weather::indexToRefId(0));
    }

    WeatherManager::~WeatherManager()
    {
        stopSounds();
    }

    void WeatherManager::changeWeather(ESM::RefId regionID, ESM::RefId weatherID)
    {
        // In Morrowind, this seems to have the following behavior, when applied to the current region:
        // - When there is no transition in progress, start transitioning to the new weather.
        // - If there is a transition in progress, queue up the transition and process it when the current one
        // completes.
        // - If there is a transition in progress, and a queued transition, overwrite the queued transition.
        // - If multiple calls to ChangeWeather are made while paused (console up), only the last call will be used,
        //   meaning that if there was no transition in progress, only the last ChangeWeather will be processed.
        // If the region isn't current, Morrowind will store the new weather for the region in question.

        if (const Weather* weather = mWeatherStore->search(weatherID))
        {
            auto rIt = mRegions.find(regionID);
            if (rIt != mRegions.end())
            {
                rIt->second.setWeather(weather->mId);
                regionalWeatherChanged(rIt->first, rIt->second);
            }
        }
    }

    void WeatherManager::modRegion(ESM::RefId regionID, const std::map<ESM::RefId, uint8_t>& chances)
    {
        // Sets the region's probability for various weather patterns. Note that this appears to be saved permanently.
        // In Morrowind, this seems to have the following behavior when applied to the current region:
        // - If the region supports the current weather, no change in current weather occurs.
        // - If the region no longer supports the current weather, and there is no transition in progress, begin to
        //   transition to a new supported weather type.
        // - If the region no longer supports the current weather, and there is a transition in progress, queue a
        //   transition to a new supported weather type.

        auto it = mRegions.find(regionID);
        if (it != mRegions.end())
        {
            it->second.setChances(chances, *mWeatherStore);
            regionalWeatherChanged(it->first, it->second);
        }
    }

    const std::map<ESM::RefId, uint8_t>& WeatherManager::getRegionChances(ESM::RefId regionID) const
    {
        return mRegions.at(regionID).getChances();
    }

    void WeatherManager::playerTeleported(const ESM::RefId& playerRegion, bool isExterior)
    {
        // If the player teleports to an outdoors cell in a new region (for instance, by travelling), the weather needs
        // to be changed immediately, and any transitions for the previous region discarded.
        {
            auto it = mRegions.find(playerRegion);
            if (it != mRegions.end() && playerRegion != mCurrentRegion)
            {
                mCurrentRegion = playerRegion;
                forceWeather(it->second.getWeather(*mWeatherStore));
            }
        }
    }

    float WeatherManager::calculateWindSpeed(const Weather& weather, float currentSpeed)
    {
        float targetSpeed = std::min(8.0f * weather.mWindSpeed, 70.f);
        if (currentSpeed == 0.f)
            currentSpeed = targetSpeed;

        float multiplier = weather.mRainEffect.empty() ? 1.f : 0.5f;
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        float updatedSpeed = (Misc::Rng::rollClosedProbability(prng) - 0.5f) * multiplier * targetSpeed + currentSpeed;

        if (updatedSpeed > 0.5f * targetSpeed && updatedSpeed < 2.f * targetSpeed)
            currentSpeed = updatedSpeed;

        return currentSpeed;
    }

    void WeatherManager::update(float duration, bool paused, const TimeStamp& time, bool isExterior)
    {
        MWWorld::ConstPtr player = MWMechanics::getPlayer();

        // The sky's clock and not the frame's, so a sped-up `timescale` carries the crossings and
        // the thunder with the sun. `Sky::skyStep` says why the shipped scale is real time.
        duration = Sky::skyStep(duration, MWBase::Environment::get().getWorld()->getTimeManager()->getGameTimeScale());

        if (!paused || mFastForward)
        {
            // Add new transitions when either the player's current external region changes.
            if (updateWeatherTime() || updateWeatherRegion(player.getCell()->getCell()->getRegion()))
            {
                auto it = mRegions.find(mCurrentRegion);
                if (it != mRegions.end())
                {
                    addWeatherTransition(it->second.getWeather(*mWeatherStore));
                }
            }

            updateWeatherTransitions(duration);
        }

        bool isDay = time.getHour() >= mSunriseTime && time.getHour() <= mSky.mTimes.mNightStart;
        if (isExterior && !isDay)
            mNightDayMode = ExteriorNight;
        else if (!isExterior && isDay && mWeatherStore->find(mCurrentWeather)->mGlareView >= 0.5f)
            mNightDayMode = InteriorDay;
        else
            mNightDayMode = Default;

        mSky.mOutdoors = isExterior;
        if (!isExterior)
        {
            mRendering.setSkyEnabled(false);
            stopSounds();
            mWindSpeed = 0.f;
            mCurrentWindSpeed = 0.f;
            mNextWindSpeed = 0.f;
            return;
        }

        calculateWeatherResult(time.getHour(), duration, paused);

        if (!paused)
        {
            mWindSpeed = mSky.mWeather.mWindSpeed;
            mCurrentWindSpeed = mSky.mWeather.mCurrentWindSpeed;
            mNextWindSpeed = mSky.mWeather.mNextWindSpeed;
        }

        mIsStorm = mSky.mWeather.mIsStorm;

        // For some reason Ash Storm is not considered as a precipitation weather in game
        mPrecipitation = !(mSky.mWeather.mParticleEffect.empty() && mSky.mWeather.mRainEffect.empty())
            && mSky.mWeather.mParticleEffect != Settings::models().mWeatherashcloud.get();

        mStormDirection = calculateStormDirection(mSky.mWeather.mParticleEffect);
        mSky.mStormParticleDirection = mStormDirection;

        // disable sun during night
        mSky.mSunUp = Sky::sunUp(time.getHour(), mSky.mTimes);

        // Update the sun direction.  Run it east to west at a fixed angle from overhead.
        // The sun's speed at day and night may differ, since mSunriseTime and mNightStart
        // mark when the sun is level with the horizon.
        {
            // Shift times into a 24-hour window beginning at mSunriseTime...
            float adjustedHour = time.getHour();
            float adjustedNightStart = mSky.mTimes.mNightStart;
            if (time.getHour() < mSunriseTime)
                adjustedHour += 24.f;
            if (mSky.mTimes.mNightStart < mSunriseTime)
                adjustedNightStart += 24.f;

            const bool isNight = adjustedHour >= adjustedNightStart;
            const float dayDuration = adjustedNightStart - mSunriseTime;
            const float nightDuration = 24.f - dayDuration;

            float orbit;
            if (!isNight)
            {
                float t = (adjustedHour - mSunriseTime) / dayDuration;
                orbit = 1.f - 2.f * t;
            }
            else
            {
                float t = (adjustedHour - adjustedNightStart) / nightDuration;
                orbit = 2.f * t - 1.f;
            }

            // Hardcoded constant from Morrowind
            const osg::Vec3f sunDir(-400.f * orbit, 75.f, -100.f);
            mRendering.setSunDirection(sunDir);
            mRendering.setNight(isNight);
            mSky.mSunDirection = sunDir;
            mSky.mNight = isNight;
        }

        float underwaterFog = mUnderwaterFog.getValue(time.getHour(), mSky.mTimes, "Fog");

        float peakHour = mSunriseTime + (mSky.mTimes.mNightStart - mSunriseTime) / 2;
        float glareFade = 1.f;
        if (time.getHour() < mSunriseTime || time.getHour() > mSky.mTimes.mNightStart)
            glareFade = 0.f;
        else if (time.getHour() < peakHour)
            glareFade = 1.f - (peakHour - time.getHour()) / (peakHour - mSunriseTime);
        else
            glareFade = 1.f - (time.getHour() - peakHour) / (mSky.mTimes.mNightStart - peakHour);

        mSky.mGlareFade = glareFade;

        mSky.mMoons[0] = mMasser.calculateState(time);
        mSky.mMoons[1] = mSecunda.calculateState(time);

        mRendering.configureFog(mSky.mWeather.mFogDepth, underwaterFog, mSky.mWeather.mDLFogFactor,
            mSky.mWeather.mDLFogOffset / 100.0f, mSky.mWeather.mFogColor);
        mRendering.setAmbientColour(mSky.mWeather.mAmbientColor);
        mRendering.setSunColour(mSky.mWeather.mSunColor, mSky.mWeather.mSunColor, mSky.mWeather.mGlareView * glareFade);

        // Play sounds
        if (mPlayingAmbientSoundID != mSky.mWeather.mAmbientLoopSoundID)
        {
            if (mAmbientSound)
            {
                MWBase::Environment::get().getSoundManager()->stopSound(mAmbientSound);
                mAmbientSound = nullptr;
            }
            if (!mSky.mWeather.mAmbientLoopSoundID.empty())
                mAmbientSound
                    = MWBase::Environment::get().getSoundManager()->playSound(mSky.mWeather.mAmbientLoopSoundID,
                        mSky.mWeather.mAmbientSoundVolume, 1.0, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
            mPlayingAmbientSoundID = mSky.mWeather.mAmbientLoopSoundID;
        }
        else if (mAmbientSound)
            mAmbientSound->setVolume(mSky.mWeather.mAmbientSoundVolume);

        if (mPlayingRainSoundID != mSky.mWeather.mRainLoopSoundID)
        {
            if (mRainSound)
            {
                MWBase::Environment::get().getSoundManager()->stopSound(mRainSound);
                mRainSound = nullptr;
            }
            if (!mSky.mWeather.mRainLoopSoundID.empty())
                mRainSound = MWBase::Environment::get().getSoundManager()->playSound(mSky.mWeather.mRainLoopSoundID,
                    mSky.mWeather.mAmbientSoundVolume, 1.0, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
            mPlayingRainSoundID = mSky.mWeather.mRainLoopSoundID;
        }
        else if (mRainSound)
            mRainSound->setVolume(mSky.mWeather.mAmbientSoundVolume);
    }

    void WeatherManager::stopSounds()
    {
        MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
        if (mAmbientSound)
        {
            sndMgr->stopSound(mAmbientSound);
            mAmbientSound = nullptr;
        }
        mPlayingAmbientSoundID = ESM::RefId();

        if (mRainSound)
        {
            sndMgr->stopSound(mRainSound);
            mRainSound = nullptr;
        }
        mPlayingRainSoundID = ESM::RefId();

        const auto stopThunder = [&](ESM::RefId weatherId) {
            const Weather* weather = mWeatherStore->search(weatherId);
            if (!weather)
                return;
            const ConstPtr target;
            for (const ESM::RefId& soundId : weather->mThunderSoundID)
                if (!soundId.empty() && sndMgr->getSoundPlaying(target, soundId))
                    sndMgr->stopSound3D(target, soundId);
        };
        stopThunder(mCurrentWeather);
        if (inTransition())
            stopThunder(mNextWeather);
    }

    float WeatherManager::getWindSpeed() const
    {
        return mWindSpeed;
    }

    bool WeatherManager::isInStorm() const
    {
        return mIsStorm;
    }

    osg::Vec3f WeatherManager::getStormDirection() const
    {
        return mStormDirection;
    }

    void WeatherManager::advanceTime(double hours, bool incremental)
    {
        // In Morrowind, when the player sleeps/waits, serves jail time, travels, or trains, all weather transitions are
        // immediately applied, regardless of whatever transition time might have been remaining.
        mTimePassed += static_cast<float>(hours);
        mFastForward = !incremental ? true : mFastForward;
    }

    NightDayMode WeatherManager::getNightDayMode() const
    {
        return mNightDayMode;
    }

    bool WeatherManager::useTorches(float hour) const
    {
        bool isDark = hour < mSunriseTime || hour > mSky.mTimes.mNightStart;

        return isDark && !mPrecipitation;
    }

    float WeatherManager::getSunPercentage(float hour) const
    {
        if (hour <= mSky.mTimes.mNightEnd || hour >= mSky.mTimes.mNightStart)
            return 0.f;
        else if (hour <= mSky.mTimes.mDayStart)
            return (hour - mSky.mTimes.mNightEnd) / mSunriseDuration;
        else if (hour > mSky.mTimes.mDayEnd)
            return 1.f - ((hour - mSky.mTimes.mDayEnd) / mSunsetDuration);
        return 1.f;
    }

    float WeatherManager::getSunVisibility() const
    {
        const Weather& currentWeather = *mWeatherStore->find(mCurrentWeather);
        if (inTransition())
        {
            const Weather& nextWeather = *mWeatherStore->find(mNextWeather);
            if (mTransitionFactor < nextWeather.mCloudsMaximumPercent)
            {
                float t = mTransitionFactor / nextWeather.mCloudsMaximumPercent;
                return (1.f - t) * currentWeather.mGlareView + t * nextWeather.mGlareView;
            }
        }
        return currentWeather.mGlareView;
    }

    void WeatherManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        ESM::WeatherState state;
        state.mCurrentRegion = mCurrentRegion;
        state.mTimePassed = mTimePassed;
        state.mFastForward = mFastForward;
        state.mWeatherUpdateTime = mWeatherUpdateTime;
        state.mTransitionFactor = mTransitionFactor;
        state.mCurrentWeather = mCurrentWeather;
        state.mNextWeather = mNextWeather;
        state.mQueuedWeather = mQueuedWeather;

        auto it = mRegions.begin();
        for (; it != mRegions.end(); ++it)
        {
            state.mRegions.insert(std::make_pair(it->first, it->second));
        }

        writer.startRecord(ESM::REC_WTHR);
        state.save(writer);
        writer.endRecord(ESM::REC_WTHR);
    }

    bool WeatherManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (ESM::REC_WTHR == type)
        {
            ESM::WeatherState state;
            state.load(reader);

            std::swap(mCurrentRegion, state.mCurrentRegion);
            mTimePassed = state.mTimePassed;
            mFastForward = state.mFastForward;
            mWeatherUpdateTime = state.mWeatherUpdateTime;
            mTransitionFactor = state.mTransitionFactor;
            mCurrentWeather = state.mCurrentWeather;
            mNextWeather = state.mNextWeather;
            mQueuedWeather = state.mQueuedWeather;

            mRegions.clear();
            importRegions();

            for (auto it = state.mRegions.begin(); it != state.mRegions.end(); ++it)
            {
                auto found = mRegions.find(it->first);
                if (found != mRegions.end())
                {
                    found->second = RegionWeather(it->second);
                }
            }

            return true;
        }

        return false;
    }

    void WeatherManager::clear()
    {
        stopSounds();

        mCurrentRegion = ESM::RefId();
        mTimePassed = 0.0f;
        mWeatherUpdateTime = 0.0f;
        forceWeather(ESM::Weather::indexToRefId(0));
        mRegions.clear();
        importRegions();
    }

    inline void WeatherManager::importRegions()
    {
        for (const ESM::Region& region : mStore.get<ESM::Region>())
        {
            mRegions.insert(std::make_pair(region.mId, RegionWeather(region)));
        }
    }

    inline void WeatherManager::regionalWeatherChanged(const ESM::RefId& regionID, RegionWeather& region)
    {
        // If the region is current, then add a weather transition for it.
        MWWorld::ConstPtr player = MWMechanics::getPlayer();
        if (player.isInCell())
        {
            if (regionID == mCurrentRegion)
            {
                addWeatherTransition(region.getWeather(*mWeatherStore));
            }
        }
    }

    inline bool WeatherManager::updateWeatherTime()
    {
        mWeatherUpdateTime -= mTimePassed;
        mTimePassed = 0.0f;
        if (mWeatherUpdateTime <= 0.0f)
        {
            // Expire all regional weather, so that any call to getWeather() will return a new weather ID.
            auto it = mRegions.begin();
            for (; it != mRegions.end(); ++it)
            {
                it->second.setWeather({});
            }

            mWeatherUpdateTime += mHoursBetweenWeatherChanges;

            return true;
        }

        return false;
    }

    inline bool WeatherManager::updateWeatherRegion(const ESM::RefId& playerRegion)
    {
        if (!playerRegion.empty() && playerRegion != mCurrentRegion)
        {
            mCurrentRegion = playerRegion;

            return true;
        }

        return false;
    }

    inline void WeatherManager::updateWeatherTransitions(const float elapsedRealSeconds)
    {
        // When a player chooses to train, wait, or serves jail time, any transitions will be fast forwarded to the last
        // weather type set, regardless of the remaining transition time.
        if (!mFastForward && inTransition())
        {
            const float delta = mWeatherStore->find(mNextWeather)->transitionDelta();
            mTransitionFactor -= elapsedRealSeconds * delta;
            if (mTransitionFactor <= 0.0f)
            {
                mCurrentWeather = mNextWeather;
                mNextWeather = mQueuedWeather;
                mQueuedWeather = {};

                // We may have begun processing the queued transition, so we need to apply the remaining time towards
                // it.
                if (inTransition())
                {
                    const float newDelta = mWeatherStore->find(mNextWeather)->transitionDelta();
                    const float remainingSeconds = -(mTransitionFactor / delta);
                    mTransitionFactor = 1.0f - (remainingSeconds * newDelta);
                }
                else
                {
                    mTransitionFactor = 0.0f;
                }
            }
        }
        else
        {
            if (!mQueuedWeather.empty())
            {
                mCurrentWeather = mQueuedWeather;
            }
            else if (!mNextWeather.empty())
            {
                mCurrentWeather = mNextWeather;
            }

            mNextWeather = {};
            mQueuedWeather = {};
            mFastForward = false;
        }
    }

    inline void WeatherManager::forceWeather(ESM::RefId weatherID)
    {
        mTransitionFactor = 0.0f;
        mCurrentWeather = weatherID;
        mNextWeather = {};
        mQueuedWeather = {};
    }

    inline bool WeatherManager::inTransition() const
    {
        return !mNextWeather.empty();
    }

    inline void WeatherManager::addWeatherTransition(ESM::RefId weatherID)
    {
        // In order to work like ChangeWeather expects, this method begins transitioning to the new weather immediately
        // if no transition is in progress, otherwise it queues it to be transitioned.

        assert(!weatherID.empty());

        if (!inTransition() && (weatherID != mCurrentWeather))
        {
            mNextWeather = weatherID;
            mTransitionFactor = 1.0f;
        }
        else if (inTransition() && (weatherID != mNextWeather))
        {
            mQueuedWeather = weatherID;
        }
    }

    inline void WeatherManager::calculateWeatherResult(
        const float gameHour, const float elapsedSeconds, const bool isPaused)
    {
        float flash = 0.0f;
        if (!inTransition())
        {
            Weather& current = *mWeatherStore->find(mCurrentWeather);
            calculateResult(current, gameHour);
            flash = current.calculateThunder(1.0f, elapsedSeconds, isPaused);
        }
        else
        {
            calculateTransitionResult(1 - mTransitionFactor, gameHour);
            Weather& current = *mWeatherStore->find(mCurrentWeather);
            float currentFlash = current.calculateThunder(mTransitionFactor, elapsedSeconds, isPaused);
            Weather& next = *mWeatherStore->find(mNextWeather);
            float nextFlash = next.calculateThunder(1 - mTransitionFactor, elapsedSeconds, isPaused);
            flash = currentFlash + nextFlash;
        }
        osg::Vec4f flashColor(flash, flash, flash, 0.0f);

        mSky.mWeather.mFogColor += flashColor;
        mSky.mWeather.mAmbientColor += flashColor;
        mSky.mWeather.mSunColor += flashColor;
    }

    inline void WeatherManager::calculateResult(const Weather& current, const float gameHour)
    {
        mSky.mWeather.mCloudTexture = current.mCloudTexture;
        mSky.mWeather.mCloudBlendFactor = 0;
        mSky.mWeather.mNextWindSpeed = 0;
        mSky.mWeather.mWindSpeed = mSky.mWeather.mCurrentWindSpeed = calculateWindSpeed(current, mWindSpeed);
        mSky.mWeather.mBaseWindSpeed = current.mWindSpeed;

        mSky.mWeather.mCloudSpeed = current.mCloudSpeed;
        mSky.mWeather.mGlareView = current.mGlareView;
        mSky.mWeather.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
        mSky.mWeather.mRainLoopSoundID = current.mRainLoopSoundID;
        mSky.mWeather.mAmbientSoundVolume = 1.f;
        mSky.mWeather.mPrecipitationAlpha = 1.f;

        mSky.mWeather.mIsStorm = current.mIsStorm;

        mSky.mWeather.mRainSpeed = current.mRainSpeed;
        mSky.mWeather.mRainEntranceSpeed = current.mRainEntranceSpeed;
        mSky.mWeather.mRainDiameter = current.mRainDiameter;
        mSky.mWeather.mRainMinHeight = current.mRainMinHeight;
        mSky.mWeather.mRainMaxHeight = current.mRainMaxHeight;
        mSky.mWeather.mRainMaxRaindrops = current.mRainMaxRaindrops;

        mSky.mWeather.mParticleEffect = current.mParticleEffect;
        mSky.mWeather.mRainEffect = current.mRainEffect;

        mSky.mWeather.mNight = (gameHour < mSunriseTime
            || gameHour
                > mSky.mTimes.mNightStart + mSky.mTimes.mStarsPostSunsetStart - mSky.mTimes.mStarsFadingDuration);

        mSky.mWeather.mFogDepth = current.mLandFogDepth.getValue(gameHour, mSky.mTimes, "Fog");
        mSky.mWeather.mFogColor = current.mFogColor.getValue(gameHour, mSky.mTimes, "Fog");
        mSky.mWeather.mAmbientColor = current.mAmbientColor.getValue(gameHour, mSky.mTimes, "Ambient");
        mSky.mWeather.mSunColor = current.mSunColor.getValue(gameHour, mSky.mTimes, "Sun");
        mSky.mWeather.mSkyColor = current.mSkyColor.getValue(gameHour, mSky.mTimes, "Sky");
        mSky.mWeather.mNightFade = mNightFade.getValue(gameHour, mSky.mTimes, "Stars");
        mSky.mWeather.mDLFogFactor = current.mDL.FogFactor;
        mSky.mWeather.mDLFogOffset = current.mDL.FogOffset;

        WeatherSetting setting = mSky.mTimes.getSetting("Sun");
        float preSunsetTime = setting.mPreSunsetTime;

        if (gameHour >= mSky.mTimes.mDayEnd - preSunsetTime)
        {
            float factor = 1.f;
            if (preSunsetTime > 0)
                factor = (gameHour - (mSky.mTimes.mDayEnd - preSunsetTime)) / preSunsetTime;
            factor = std::min(1.f, factor);
            mSky.mWeather.mSunDiscColor = lerp(osg::Vec4f(1, 1, 1, 1), current.mSunDiscSunsetColor, factor);
            // The SunDiscSunsetColor in the INI isn't exactly the resulting color on screen, most likely because
            // MW applied the color to the ambient term as well. After the ambient and emissive terms are added
            // together, the fixed pipeline would then clamp the total lighting to (1,1,1). A noticeable change in color
            // tone can be observed when only one of the color components gets clamped. Unfortunately that means we
            // can't use the INI color as is, have to replicate the above nonsense.
            mSky.mWeather.mSunDiscColor = mSky.mWeather.mSunDiscColor
                + osg::componentMultiply(mSky.mWeather.mSunDiscColor, mSky.mWeather.mAmbientColor);
            for (int i = 0; i < 3; ++i)
                mSky.mWeather.mSunDiscColor[i] = std::min(1.f, mSky.mWeather.mSunDiscColor[i]);
        }
        else
            mSky.mWeather.mSunDiscColor = osg::Vec4f(1, 1, 1, 1);

        mSky.mWeather.mSunDiscColor.a() = Sky::sunDiscAlpha(gameHour, mSky.mTimes);

        mSky.mWeather.mStormDirection = calculateStormDirection(mSky.mWeather.mParticleEffect);
    }

    inline void WeatherManager::calculateTransitionResult(const float factor, const float gameHour)
    {
        const Weather& currentWeather = *mWeatherStore->find(mCurrentWeather);
        const Weather& nextWeather = *mWeatherStore->find(mNextWeather);
        calculateResult(currentWeather, gameHour);
        const MWRender::WeatherResult current = mSky.mWeather;
        calculateResult(nextWeather, gameHour);
        const MWRender::WeatherResult other = mSky.mWeather;

        mSky.mWeather.mStormDirection = current.mStormDirection;
        mSky.mWeather.mNextStormDirection = other.mStormDirection;

        mSky.mWeather.mCloudTexture = current.mCloudTexture;
        mSky.mWeather.mNextCloudTexture = other.mCloudTexture;
        mSky.mWeather.mCloudBlendFactor = nextWeather.cloudBlendFactor(factor);

        mSky.mWeather.mFogColor = lerp(current.mFogColor, other.mFogColor, factor);
        mSky.mWeather.mSunColor = lerp(current.mSunColor, other.mSunColor, factor);
        mSky.mWeather.mSkyColor = lerp(current.mSkyColor, other.mSkyColor, factor);

        mSky.mWeather.mAmbientColor = lerp(current.mAmbientColor, other.mAmbientColor, factor);
        mSky.mWeather.mSunDiscColor = lerp(current.mSunDiscColor, other.mSunDiscColor, factor);
        mSky.mWeather.mFogDepth = lerp(current.mFogDepth, other.mFogDepth, factor);
        mSky.mWeather.mDLFogFactor = lerp(current.mDLFogFactor, other.mDLFogFactor, factor);
        mSky.mWeather.mDLFogOffset = lerp(current.mDLFogOffset, other.mDLFogOffset, factor);

        mSky.mWeather.mCurrentWindSpeed = calculateWindSpeed(currentWeather, mCurrentWindSpeed);
        mSky.mWeather.mNextWindSpeed = calculateWindSpeed(nextWeather, mNextWindSpeed);
        mSky.mWeather.mBaseWindSpeed = lerp(current.mBaseWindSpeed, other.mBaseWindSpeed, factor);

        mSky.mWeather.mWindSpeed = lerp(mSky.mWeather.mCurrentWindSpeed, mSky.mWeather.mNextWindSpeed, factor);
        mSky.mWeather.mCloudSpeed = lerp(current.mCloudSpeed, other.mCloudSpeed, factor);
        mSky.mWeather.mGlareView = lerp(current.mGlareView, other.mGlareView, factor);
        mSky.mWeather.mNightFade = lerp(current.mNightFade, other.mNightFade, factor);

        mSky.mWeather.mNight = current.mNight;

        float threshold = nextWeather.mRainThreshold;
        if (threshold <= 0.f)
            threshold = 0.5f;

        if (factor < threshold)
        {
            mSky.mWeather.mIsStorm = current.mIsStorm;
            mSky.mWeather.mParticleEffect = current.mParticleEffect;
            mSky.mWeather.mRainEffect = current.mRainEffect;
            mSky.mWeather.mRainSpeed = current.mRainSpeed;
            mSky.mWeather.mRainEntranceSpeed = current.mRainEntranceSpeed;
            mSky.mWeather.mAmbientSoundVolume = 1.f - factor / threshold;
            mSky.mWeather.mPrecipitationAlpha = mSky.mWeather.mAmbientSoundVolume;
            mSky.mWeather.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
            mSky.mWeather.mRainLoopSoundID = current.mRainLoopSoundID;
            mSky.mWeather.mRainDiameter = current.mRainDiameter;
            mSky.mWeather.mRainMinHeight = current.mRainMinHeight;
            mSky.mWeather.mRainMaxHeight = current.mRainMaxHeight;
            mSky.mWeather.mRainMaxRaindrops = current.mRainMaxRaindrops;
        }
        else
        {
            mSky.mWeather.mIsStorm = other.mIsStorm;
            mSky.mWeather.mParticleEffect = other.mParticleEffect;
            mSky.mWeather.mRainEffect = other.mRainEffect;
            mSky.mWeather.mRainSpeed = other.mRainSpeed;
            mSky.mWeather.mRainEntranceSpeed = other.mRainEntranceSpeed;
            mSky.mWeather.mAmbientSoundVolume = (factor - threshold) / (1 - threshold);
            mSky.mWeather.mPrecipitationAlpha = mSky.mWeather.mAmbientSoundVolume;
            mSky.mWeather.mAmbientLoopSoundID = other.mAmbientLoopSoundID;
            mSky.mWeather.mRainLoopSoundID = other.mRainLoopSoundID;

            mSky.mWeather.mRainDiameter = other.mRainDiameter;
            mSky.mWeather.mRainMinHeight = other.mRainMinHeight;
            mSky.mWeather.mRainMaxHeight = other.mRainMaxHeight;
            mSky.mWeather.mRainMaxRaindrops = other.mRainMaxRaindrops;
        }
    }
}
