#include "weather.hpp"

#include <components/weather/downpour.hpp>

#include <components/esm/stringrefid.hpp>
#include <components/settings/values.hpp>

#include <components/misc/rng.hpp>

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
#include "esmstore.hpp"
#include "player.hpp"

#include <algorithm>
#include <cmath>

namespace MWWorld
{
    namespace
    {
        static const int invalidWeatherID = -1;

        // Linear interpolation between x and y. Still here because the weather's own transition
        // between two skies uses it; the time-of-day ramp that used to share them has moved to
        // `Sky::TimeOfDayInterpolator` and carries its own.
        float lerp(float x, float y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }

        osg::Vec4f lerp(const osg::Vec4f& x, const osg::Vec4f& y, float factor)
        {
            return x * (1 - factor) + y * factor;
        }

        /// Which way this weather's effect is driving, aimed at wherever the player is standing.
        ///
        /// **The rule is `::Weather::stormDirection` and lives beside the rest of what a weather
        /// drops**, because a ray tracer aims the same storm at the same observer and a harness
        /// that renders one aims it at a camera. What is left here is the one thing this file knows
        /// and neither of those does: which body counts as the observer.
        osg::Vec3f calculateStormDirection(const std::string& particleEffect)
        {
            return ::Weather::stormDirection(
                particleEffect, MWMechanics::getPlayer().getRefData().getPosition().asVec3());
        }
    }

    Weather::Weather(ESM::RefId id, int scriptId, const std::string& name, float stormWindSpeed, float rainSpeed,
        float dlFactor, float dlOffset)
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
        , mCloudSpeed(Fallback::Map::getFloat("Weather_" + name + "_Cloud_Speed"))
        , mGlareView(Fallback::Map::getFloat("Weather_" + name + "_Glare_View"))
        , mDownpour(::Weather::downpourAt(name, stormWindSpeed, rainSpeed))
        , mRainThreshold(Fallback::Map::getFloat("Weather_" + name + "_Rain_Threshold"))
        , mStormDirection(::Weather::defaultStormDirection())
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

        if (!mDownpour.mRainEffect.empty()) // NOTE: in vanilla, the weathers with rain seem to be hardcoded;
                                            // changing Using_Precip has no effect
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
        // `Sky::cloudBlend`'s, because a renderer with no weather system to run has to cross its sky
        // on the same curve — and because the case that curve gets wrong is a black sky.
        return Sky::cloudBlend(transitionRatio, mCloudsMaximumPercent);
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
        : mWeather(invalidWeatherID)
        , mChances(region.mData.mProbabilities.begin(), region.mData.mProbabilities.end())
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

    void RegionWeather::setChances(std::span<const uint8_t> chances)
    {
        mChances.assign(chances.begin(), chances.end());

        // Regional weather no longer supports the current type, select a new weather pattern.
        if ((static_cast<size_t>(mWeather) >= mChances.size()) || (mChances[mWeather] == 0))
        {
            chooseNewWeather();
        }
    }

    std::span<const uint8_t> RegionWeather::getChances() const
    {
        return mChances;
    }

    void RegionWeather::setWeather(int weatherID)
    {
        mWeather = weatherID;
    }

    int RegionWeather::getWeather()
    {
        // If the region weather was already set (by ChangeWeather, or by a previous call) then just return that value.
        // Note that the region weather will be expired periodically when the weather update timer expires.
        if (mWeather == invalidWeatherID)
        {
            chooseNewWeather();
        }

        return mWeather;
    }

    void RegionWeather::chooseNewWeather()
    {
        // All probabilities must add to 100 (responsibility of the user).
        // If chances A and B has values 30 and 70 then by generating 100 numbers 1..100, 30% will be lesser or equal 30
        // and 70% will be greater than 30 (in theory).
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        unsigned int chance = static_cast<unsigned int>(Misc::Rng::rollDice(100, prng) + 1); // 1..100
        unsigned int sum = 0;
        for (size_t i = 0; i < mChances.size(); ++i)
        {
            sum += mChances[i];
            if (chance <= sum)
            {
                mWeather = static_cast<int>(i);
                return;
            }
        }

        // if we hit this path then the chances don't add to 100, choose a default weather instead
        mWeather = 0;
    }

    MWRender::MoonState MoonModel::calculateState(const TimeStamp& gameTime) const
    {
        const Sky::MoonMoment moment = mModel.at(gameTime.getDay(), gameTime.getHour());

        // **The two orders have to agree and neither owns the other**, so this pins them rather than
        // trusting them: the phases are eight even steps counted from full, which is what makes the
        // index an angle for whoever wants one.
        static_assert(static_cast<int>(Sky::MoonPhase::Full) == static_cast<int>(MWRender::MoonState::Phase::Full));
        static_assert(static_cast<int>(Sky::MoonPhase::New) == static_cast<int>(MWRender::MoonState::Phase::New));
        static_assert(static_cast<int>(Sky::MoonPhase::WaxingGibbous)
            == static_cast<int>(MWRender::MoonState::Phase::WaxingGibbous));

        return MWRender::MoonState{
            .mRotationFromHorizon = moment.mAlongArc,

            // Reverse engineered from Morrowind's scene graph rotation matrices.
            .mRotationFromNorth = moment.mAxisOffset,
            .mPhase = static_cast<MWRender::MoonState::Phase>(moment.mPhase),
            .mShadowBlend = moment.mShadowBlend,
            .mMoonAlpha = moment.mAlpha,
        };
    }

    std::vector<Moon> WeatherManager::getCurrentMoons(const TimeStamp& time) const
    {
        const auto makeMoon = [](std::string_view name, const MoonModel& model, const TimeStamp& timestamp) {
            const MWRender::MoonState state = model.calculateState(timestamp);
            return Moon{ name, state.mPhase, MWRender::MoonState::phaseToInt(state.mPhase), state.mMoonAlpha };
        };

        return { makeMoon("Masser", mMasser, time), makeMoon("Secunda", mSecunda, time) };
    }

    WeatherManager::WeatherManager(MWRender::RenderingManager& rendering, MWWorld::ESMStore& store)
        : mStore(store)
        , mRendering(rendering)
        , mSunriseTime(Fallback::Map::getFloat("Weather_Sunrise_Time"))
        , mSunsetTime(Fallback::Map::getFloat("Weather_Sunset_Time"))
        , mSunriseDuration(Fallback::Map::getFloat("Weather_Sunrise_Duration"))
        , mSunsetDuration(Fallback::Map::getFloat("Weather_Sunset_Duration"))
        , mSunPreSunsetTime(Fallback::Map::getFloat("Weather_Sun_Pre-Sunset_Time"))
        , mNightFade(0, 0, 0, 1)
        , mHoursBetweenWeatherChanges(Fallback::Map::getFloat("Weather_Hours_Between_Weather_Changes"))
        , mRainSpeed(Fallback::Map::getFloat("Weather_Precip_Gravity"))
        , mUnderwaterFog(Fallback::Map::getFloat("Water_UnderwaterSunriseFog"),
              Fallback::Map::getFloat("Water_UnderwaterDayFog"), Fallback::Map::getFloat("Water_UnderwaterSunsetFog"),
              Fallback::Map::getFloat("Water_UnderwaterNightFog"))
        , mWeatherSettings()
        , mMasser("Masser")
        , mSecunda("Secunda")
        , mWindSpeed(0.f)
        , mCurrentWindSpeed(0.f)
        , mNextWindSpeed(0.f)
        , mIsStorm(false)
        , mPrecipitation(false)
        , mStormDirection(::Weather::defaultStormDirection())
        , mCurrentRegion()
        , mTimePassed(0)
        , mFastForward(false)
        , mWeatherUpdateTime(mHoursBetweenWeatherChanges)
        , mTransitionFactor(0)
        , mNightDayMode(Default)
        , mCurrentWeather(0)
        , mNextWeather(0)
        , mQueuedWeather(0)
        , mRegions()
        , mResult()
    {
        mTimeSettings.mNightStart = mSunsetTime + mSunsetDuration;
        mTimeSettings.mNightEnd = mSunriseTime;
        mTimeSettings.mDayStart = mSunriseTime + mSunriseDuration;
        mTimeSettings.mDayEnd = mSunsetTime;

        mTimeSettings.addSetting("Sky");
        mTimeSettings.addSetting("Ambient");
        mTimeSettings.addSetting("Fog");
        mTimeSettings.addSetting("Sun");

        // Morrowind handles stars settings differently for other ones
        mTimeSettings.mStarsPostSunsetStart = Fallback::Map::getFloat("Weather_Stars_Post-Sunset_Start");
        mTimeSettings.mStarsPreSunriseFinish = Fallback::Map::getFloat("Weather_Stars_Pre-Sunrise_Finish");
        mTimeSettings.mStarsFadingDuration = Fallback::Map::getFloat("Weather_Stars_Fading_Duration");

        Sky::WeatherSetting starSetting = { mTimeSettings.mStarsPreSunriseFinish,
            mTimeSettings.mStarsFadingDuration - mTimeSettings.mStarsPreSunriseFinish,
            mTimeSettings.mStarsPostSunsetStart,
            mTimeSettings.mStarsFadingDuration - mTimeSettings.mStarsPostSunsetStart };

        mTimeSettings.mSunriseTransitions["Stars"] = starSetting;

        mWeatherSettings.reserve(10);
        // These distant land fog factor and offset values are the defaults MGE XE provides. Should be
        // provided by settings somewhere?
        addWeather("Clear", 1.0f, 0.0f); // 0
        addWeather("Cloudy", 0.9f, 0.0f); // 1
        addWeather("Foggy", 0.2f, 30.0f); // 2
        addWeather("Overcast", 0.7f, 0.0f); // 3
        addWeather("Rain", 0.5f, 10.0f); // 4
        addWeather("Thunderstorm", 0.5f, 20.0f); // 5
        addWeather("Ashstorm", 0.2f, 50.0f); // 6
        addWeather("Blight", 0.2f, 60.0f); // 7
        addWeather("Snow", 0.5f, 40.0f); // 8
        addWeather("Blizzard", 0.16f, 70.0f); // 9

        Store<ESM::Region>::iterator it = store.get<ESM::Region>().begin();
        for (; it != store.get<ESM::Region>().end(); ++it)
        {
            mRegions.insert(std::make_pair(it->mId, RegionWeather(*it)));
        }

        forceWeather(0);
    }

    WeatherManager::~WeatherManager()
    {
        stopSounds();
    }

    const Weather* WeatherManager::getWeather(size_t index) const
    {
        if (index < mWeatherSettings.size())
            return &mWeatherSettings[index];

        return nullptr;
    }

    const Weather* WeatherManager::getWeather(const ESM::RefId& id) const
    {
        auto it = std::find_if(
            mWeatherSettings.begin(), mWeatherSettings.end(), [id](const auto& weather) { return weather.mId == id; });

        if (it != mWeatherSettings.end())
            return &*it;

        return nullptr;
    }

    void WeatherManager::changeWeather(const ESM::RefId& regionID, const ESM::RefId& weatherID)
    {
        auto wIt = std::find_if(mWeatherSettings.begin(), mWeatherSettings.end(),
            [weatherID](const auto& weather) { return weather.mId == weatherID; });

        if (wIt != mWeatherSettings.end())
        {
            auto rIt = mRegions.find(regionID);
            if (rIt != mRegions.end())
            {
                rIt->second.setWeather(wIt->mScriptId);
                regionalWeatherChanged(rIt->first, rIt->second);
            }
        }
    }

    void WeatherManager::changeWeather(const ESM::RefId& regionID, const unsigned int weatherID)
    {
        // In Morrowind, this seems to have the following behavior, when applied to the current region:
        // - When there is no transition in progress, start transitioning to the new weather.
        // - If there is a transition in progress, queue up the transition and process it when the current one
        // completes.
        // - If there is a transition in progress, and a queued transition, overwrite the queued transition.
        // - If multiple calls to ChangeWeather are made while paused (console up), only the last call will be used,
        //   meaning that if there was no transition in progress, only the last ChangeWeather will be processed.
        // If the region isn't current, Morrowind will store the new weather for the region in question.

        if (weatherID < mWeatherSettings.size())
        {
            auto it = mRegions.find(regionID);
            if (it != mRegions.end())
            {
                it->second.setWeather(weatherID);
                regionalWeatherChanged(it->first, it->second);
            }
        }
    }

    void WeatherManager::modRegion(const ESM::RefId& regionID, std::span<const uint8_t> chances)
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
            it->second.setChances(chances);
            regionalWeatherChanged(it->first, it->second);
        }
    }

    std::span<const uint8_t> WeatherManager::getRegionChances(const ESM::RefId& regionID) const
    {
        auto it = mRegions.find(regionID);
        if (it != mRegions.end())
            return it->second.getChances();
        return {};
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
                forceWeather(it->second.getWeather());
            }
        }
    }

    float WeatherManager::calculateWindSpeed(int weatherId, float currentSpeed)
    {
        // **What it wanders about is the weather's own settled gust, and the wander is this
        // function's own.** A harness rendering one instant cannot reproduce a random walk, so what
        // it shares is where the walk starts — see `::Weather::gustSpeed`.
        const float targetSpeed = mWeatherSettings[weatherId].mDownpour.mWindSpeed;
        if (currentSpeed == 0.f)
            currentSpeed = targetSpeed;

        float multiplier = mWeatherSettings[weatherId].mDownpour.mRainEffect.empty() ? 1.f : 0.5f;
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        float updatedSpeed = (Misc::Rng::rollClosedProbability(prng) - 0.5f) * multiplier * targetSpeed + currentSpeed;

        if (updatedSpeed > 0.5f * targetSpeed && updatedSpeed < 2.f * targetSpeed)
            currentSpeed = updatedSpeed;

        return currentSpeed;
    }

    void WeatherManager::update(float duration, bool paused, const TimeStamp& time, bool isExterior)
    {
        MWWorld::ConstPtr player = MWMechanics::getPlayer();

        if (!paused || mFastForward)
        {
            // Add new transitions when either the player's current external region changes.
            if (updateWeatherTime() || updateWeatherRegion(player.getCell()->getCell()->getRegion()))
            {
                auto it = mRegions.find(mCurrentRegion);
                if (it != mRegions.end())
                {
                    addWeatherTransition(it->second.getWeather());
                }
            }

            updateWeatherTransitions(duration);
        }

        bool isDay = time.getHour() >= mSunriseTime && time.getHour() <= mTimeSettings.mNightStart;
        if (isExterior && !isDay)
            mNightDayMode = ExteriorNight;
        else if (!isExterior && isDay && mWeatherSettings[mCurrentWeather].mGlareView >= 0.5f)
            mNightDayMode = InteriorDay;
        else
            mNightDayMode = Default;

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
            mWindSpeed = mResult.mDownpour.mWindSpeed;
            mCurrentWindSpeed = mResult.mCurrentWindSpeed;
            mNextWindSpeed = mResult.mNextWindSpeed;
        }

        mIsStorm = mResult.mDownpour.mIsStorm;

        // For some reason Ash Storm is not considered as a precipitation weather in game
        mPrecipitation = !(mResult.mDownpour.mParticleEffect.empty() && mResult.mDownpour.mRainEffect.empty())
            && mResult.mDownpour.mParticleEffect != Settings::models().mWeatherashcloud.get();

        mStormDirection = calculateStormDirection(mResult.mDownpour.mParticleEffect);
        mRendering.setStormParticleDirection(mStormDirection);

        // Where the sun is, where its light goes, and how much of it there is — one reading, because
        // they are one parameter and `Sky::sunAt` is what keeps them from parting.
        {
            const Sky::SunPlacement sun = Sky::sunAt(time.getHour(), mTimeSettings);

            // The disc is switched rather than faded here because that is what a sprite takes; the
            // share it is switched on is the same number the ray tracer scales its sunlight by.
            mRendering.setSunVisible(sun.mShare > 0.f);
            mRendering.setSunDirection(sun.mDirection);
            mRendering.setNight(sun.mNight);
        }

        float underwaterFog = mUnderwaterFog.getValue(time.getHour(), mTimeSettings, "Fog");

        float peakHour = mSunriseTime + (mTimeSettings.mNightStart - mSunriseTime) / 2;
        float glareFade = 1.f;
        if (time.getHour() < mSunriseTime || time.getHour() > mTimeSettings.mNightStart)
            glareFade = 0.f;
        else if (time.getHour() < peakHour)
            glareFade = 1.f - (peakHour - time.getHour()) / (peakHour - mSunriseTime);
        else
            glareFade = 1.f - (time.getHour() - peakHour) / (mTimeSettings.mNightStart - peakHour);

        mRendering.setGlareTimeOfDayFade(glareFade);

        mRendering.setMoonStates(mMasser.calculateState(time), mSecunda.calculateState(time));

        mRendering.configureFog(
            mResult.mFogDepth, underwaterFog, mResult.mDLFogFactor, mResult.mDLFogOffset / 100.0f, mResult.mFogColor);
        mRendering.setAmbientColour(mResult.mAmbientColor);
        mRendering.setSunColour(mResult.mSunColor, mResult.mSunColor, mResult.mGlareView * glareFade);

        mRendering.setWeather(mResult);

        // Play sounds
        if (mPlayingAmbientSoundID != mResult.mAmbientLoopSoundID)
        {
            if (mAmbientSound)
            {
                MWBase::Environment::get().getSoundManager()->stopSound(mAmbientSound);
                mAmbientSound = nullptr;
            }
            if (!mResult.mAmbientLoopSoundID.empty())
                mAmbientSound = MWBase::Environment::get().getSoundManager()->playSound(mResult.mAmbientLoopSoundID,
                    mResult.mAmbientSoundVolume, 1.0, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
            mPlayingAmbientSoundID = mResult.mAmbientLoopSoundID;
        }
        else if (mAmbientSound)
            mAmbientSound->setVolume(mResult.mAmbientSoundVolume);

        if (mPlayingRainSoundID != mResult.mRainLoopSoundID)
        {
            if (mRainSound)
            {
                MWBase::Environment::get().getSoundManager()->stopSound(mRainSound);
                mRainSound = nullptr;
            }
            if (!mResult.mRainLoopSoundID.empty())
                mRainSound = MWBase::Environment::get().getSoundManager()->playSound(mResult.mRainLoopSoundID,
                    mResult.mAmbientSoundVolume, 1.0, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
            mPlayingRainSoundID = mResult.mRainLoopSoundID;
        }
        else if (mRainSound)
            mRainSound->setVolume(mResult.mAmbientSoundVolume);
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

        for (ESM::RefId soundId : mWeatherSettings[mCurrentWeather].mThunderSoundID)
            if (!soundId.empty() && sndMgr->getSoundPlaying(MWWorld::ConstPtr(), soundId))
                sndMgr->stopSound3D(MWWorld::ConstPtr(), soundId);

        if (inTransition())
            for (ESM::RefId soundId : mWeatherSettings[mNextWeather].mThunderSoundID)
                if (!soundId.empty() && sndMgr->getSoundPlaying(MWWorld::ConstPtr(), soundId))
                    sndMgr->stopSound3D(MWWorld::ConstPtr(), soundId);
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
        bool isDark = hour < mSunriseTime || hour > mTimeSettings.mNightStart;

        return isDark && !mPrecipitation;
    }

    float WeatherManager::getSunPercentage(float hour) const
    {
        if (hour <= mTimeSettings.mNightEnd || hour >= mTimeSettings.mNightStart)
            return 0.f;
        else if (hour <= mTimeSettings.mDayStart)
            return (hour - mTimeSettings.mNightEnd) / mSunriseDuration;
        else if (hour > mTimeSettings.mDayEnd)
            return 1.f - ((hour - mTimeSettings.mDayEnd) / mSunsetDuration);
        return 1.f;
    }

    float WeatherManager::getSunVisibility() const
    {
        if (inTransition() && mTransitionFactor < mWeatherSettings[mNextWeather].mCloudsMaximumPercent)
        {
            float t = mTransitionFactor / mWeatherSettings[mNextWeather].mCloudsMaximumPercent;
            return (1.f - t) * mWeatherSettings[mCurrentWeather].mGlareView
                + t * mWeatherSettings[mNextWeather].mGlareView;
        }
        return mWeatherSettings[mCurrentWeather].mGlareView;
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
        forceWeather(0);
        mRegions.clear();
        importRegions();
    }

    inline void WeatherManager::addWeather(const std::string& name, float dlFactor, float dlOffset)
    {
        static const float fStromWindSpeed = mStore.get<ESM::GameSetting>().find("fStromWindSpeed")->mValue.getFloat();
        ESM::StringRefId id(name);
        Weather weather(
            id, static_cast<int>(mWeatherSettings.size()), name, fStromWindSpeed, mRainSpeed, dlFactor, dlOffset);

        mWeatherSettings.push_back(std::move(weather));
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
                addWeatherTransition(region.getWeather());
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
                it->second.setWeather(invalidWeatherID);
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
            const float delta = mWeatherSettings[mNextWeather].transitionDelta();
            mTransitionFactor -= elapsedRealSeconds * delta;
            if (mTransitionFactor <= 0.0f)
            {
                mCurrentWeather = mNextWeather;
                mNextWeather = mQueuedWeather;
                mQueuedWeather = invalidWeatherID;

                // We may have begun processing the queued transition, so we need to apply the remaining time towards
                // it.
                if (inTransition())
                {
                    const float newDelta = mWeatherSettings[mNextWeather].transitionDelta();
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
            if (mQueuedWeather != invalidWeatherID)
            {
                mCurrentWeather = mQueuedWeather;
            }
            else if (mNextWeather != invalidWeatherID)
            {
                mCurrentWeather = mNextWeather;
            }

            mNextWeather = invalidWeatherID;
            mQueuedWeather = invalidWeatherID;
            mFastForward = false;
        }
    }

    inline void WeatherManager::forceWeather(const int weatherID)
    {
        mTransitionFactor = 0.0f;
        mCurrentWeather = weatherID;
        mNextWeather = invalidWeatherID;
        mQueuedWeather = invalidWeatherID;
    }

    inline bool WeatherManager::inTransition() const
    {
        return mNextWeather != invalidWeatherID;
    }

    inline void WeatherManager::addWeatherTransition(const int weatherID)
    {
        // In order to work like ChangeWeather expects, this method begins transitioning to the new weather immediately
        // if no transition is in progress, otherwise it queues it to be transitioned.

        assert(weatherID >= 0 && static_cast<size_t>(weatherID) < mWeatherSettings.size());

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
            calculateResult(mCurrentWeather, gameHour);
            flash = mWeatherSettings[mCurrentWeather].calculateThunder(1.0f, elapsedSeconds, isPaused);
        }
        else
        {
            calculateTransitionResult(1 - mTransitionFactor, gameHour);
            float currentFlash
                = mWeatherSettings[mCurrentWeather].calculateThunder(mTransitionFactor, elapsedSeconds, isPaused);
            float nextFlash
                = mWeatherSettings[mNextWeather].calculateThunder(1 - mTransitionFactor, elapsedSeconds, isPaused);
            flash = currentFlash + nextFlash;
        }
        osg::Vec4f flashColor(flash, flash, flash, 0.0f);

        mResult.mFogColor += flashColor;
        mResult.mAmbientColor += flashColor;
        mResult.mSunColor += flashColor;
    }

    inline void WeatherManager::calculateResult(const int weatherID, const float gameHour)
    {
        const Weather& current = mWeatherSettings[weatherID];

        mResult.mCloudTexture = current.mCloudTexture;
        mResult.mCloudBlendFactor = 0;
        mResult.mNextWindSpeed = 0;

        // **The whole of what falls, in one copy.** What is overridden below it is the two things a
        // settled weather still works out per frame: how hard it is blowing this instant, and that
        // all of it has arrived.
        mResult.mDownpour = current.mDownpour;
        mResult.mDownpour.mWindSpeed = mResult.mCurrentWindSpeed = calculateWindSpeed(weatherID, mWindSpeed);

        mResult.mCloudSpeed = current.mCloudSpeed;
        mResult.mGlareView = current.mGlareView;
        mResult.mAmbientLoopSoundID = current.mAmbientLoopSoundID;
        mResult.mRainLoopSoundID = current.mRainLoopSoundID;
        mResult.mAmbientSoundVolume = 1.f;
        mResult.mDownpour.mPrecipitationAlpha = 1.f;

        mResult.mNight = (gameHour < mSunriseTime
            || gameHour
                > mTimeSettings.mNightStart + mTimeSettings.mStarsPostSunsetStart - mTimeSettings.mStarsFadingDuration);

        mResult.mFogDepth = current.mLandFogDepth.getValue(gameHour, mTimeSettings, "Fog");
        mResult.mFogColor = current.mFogColor.getValue(gameHour, mTimeSettings, "Fog");
        mResult.mAmbientColor = current.mAmbientColor.getValue(gameHour, mTimeSettings, "Ambient");
        mResult.mSunColor = current.mSunColor.getValue(gameHour, mTimeSettings, "Sun");
        mResult.mSkyColor = current.mSkyColor.getValue(gameHour, mTimeSettings, "Sky");
        mResult.mNightFade = mNightFade.getValue(gameHour, mTimeSettings, "Stars");
        mResult.mDLFogFactor = current.mDL.FogFactor;
        mResult.mDLFogOffset = current.mDL.FogOffset;

        // What the disc is painted with, which is not `mSunColor`, and how much of the sun there is
        // to paint. `components/sky` holds both, so the ray tracer reads the same arithmetic without
        // a weather manager to ask.
        mResult.mSunDiscColor
            = osg::Vec4f(Sky::sunDiscAt(gameHour, mTimeSettings, current.mSunDiscSunsetColor, mResult.mAmbientColor),
                Sky::sunShareAt(gameHour, mTimeSettings));

        mResult.mStormDirection = calculateStormDirection(mResult.mDownpour.mParticleEffect);
    }

    inline void WeatherManager::calculateTransitionResult(const float factor, const float gameHour)
    {
        calculateResult(mCurrentWeather, gameHour);
        const MWRender::WeatherResult current = mResult;
        calculateResult(mNextWeather, gameHour);
        const MWRender::WeatherResult other = mResult;

        mResult.mStormDirection = current.mStormDirection;
        mResult.mNextStormDirection = other.mStormDirection;

        mResult.mCloudTexture = current.mCloudTexture;
        mResult.mNextCloudTexture = other.mCloudTexture;
        mResult.mCloudBlendFactor = mWeatherSettings[mNextWeather].cloudBlendFactor(factor);

        mResult.mFogColor = lerp(current.mFogColor, other.mFogColor, factor);
        mResult.mSunColor = lerp(current.mSunColor, other.mSunColor, factor);
        mResult.mSkyColor = lerp(current.mSkyColor, other.mSkyColor, factor);

        mResult.mAmbientColor = lerp(current.mAmbientColor, other.mAmbientColor, factor);
        mResult.mSunDiscColor = lerp(current.mSunDiscColor, other.mSunDiscColor, factor);
        mResult.mFogDepth = lerp(current.mFogDepth, other.mFogDepth, factor);
        mResult.mDLFogFactor = lerp(current.mDLFogFactor, other.mDLFogFactor, factor);
        mResult.mDLFogOffset = lerp(current.mDLFogOffset, other.mDLFogOffset, factor);

        mResult.mCurrentWindSpeed = calculateWindSpeed(mCurrentWeather, mCurrentWindSpeed);
        mResult.mNextWindSpeed = calculateWindSpeed(mNextWeather, mNextWindSpeed);
        mResult.mCloudSpeed = lerp(current.mCloudSpeed, other.mCloudSpeed, factor);
        mResult.mGlareView = lerp(current.mGlareView, other.mGlareView, factor);
        mResult.mNightFade = lerp(current.mNightFade, other.mNightFade, factor);

        mResult.mNight = current.mNight;

        float threshold = mWeatherSettings[mNextWeather].mRainThreshold;
        if (threshold <= 0.f)
            threshold = 0.5f;

        // **One of the two falls and the other does not, and the threshold is where it changes
        // over.** A transition's precipitation does not cross-fade between two kinds of weather:
        // what is falling fades out and what is arriving fades in, so at any instant there is one
        // `Downpour` and an alpha on it.
        const MWRender::WeatherResult& falling = factor < threshold ? current : other;

        mResult.mAmbientSoundVolume
            = factor < threshold ? 1.f - factor / threshold : (factor - threshold) / (1 - threshold);
        mResult.mAmbientLoopSoundID = falling.mAmbientLoopSoundID;
        mResult.mRainLoopSoundID = falling.mRainLoopSoundID;

        mResult.mDownpour = falling.mDownpour;
        mResult.mDownpour.mPrecipitationAlpha = mResult.mAmbientSoundVolume;

        // **The wind is the one thing that does cross**, because it is the world's rather than the
        // weather's: a gale does not stop at the moment the rain does.
        mResult.mDownpour.mWindSpeed = lerp(mResult.mCurrentWindSpeed, mResult.mNextWindSpeed, factor);
        mResult.mDownpour.mBaseWindSpeed
            = lerp(current.mDownpour.mBaseWindSpeed, other.mDownpour.mBaseWindSpeed, factor);
    }
}
