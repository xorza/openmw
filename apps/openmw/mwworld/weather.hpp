#ifndef GAME_MWWORLD_WEATHER_H
#define GAME_MWWORLD_WEATHER_H

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec4f>

#include <components/esm/refid.hpp>
#include <components/fallback/fallback.hpp>
#include <components/sky/clouds.hpp>
#include <components/sky/moonmodel.hpp>
#include <components/sky/sun.hpp>
#include <components/sky/timeofday.hpp>

#include "../mwbase/soundmanager.hpp"

#include "../mwrender/weatherresult.hpp"

namespace ESM
{
    struct Region;
    struct RegionWeatherState;
    class ESMWriter;
    class ESMReader;
}

namespace MWRender
{
    class RenderingManager;
}

namespace Loading
{
    class Listener;
}

namespace Fallback
{
    class Map;
}

namespace MWWorld
{
    class TimeStamp;

    struct Moon
    {
        std::string_view mName;
        MWRender::MoonState::Phase mPhase;
        unsigned int mPhaseValue;
        float mAlpha;
    };

    enum NightDayMode
    {
        Default = 0,
        ExteriorNight = 1,
        InteriorDay = 2
    };

    /// Defines a single weather setting (according to INI)
    class Weather
    {
    public:
        Weather(const ESM::RefId id, const int scriptId, const std::string& name, float stormWindSpeed, float rainSpeed,
            float dlFactor, float dlOffset);

        ESM::RefId mId;
        int mScriptId;
        std::string mName;
        std::string mCloudTexture;

        // Sky (atmosphere) color
        Sky::TimeOfDayInterpolator<osg::Vec4f> mSkyColor;
        // Fog color
        Sky::TimeOfDayInterpolator<osg::Vec4f> mFogColor;
        // Ambient lighting color
        Sky::TimeOfDayInterpolator<osg::Vec4f> mAmbientColor;
        // Sun (directional) lighting color
        Sky::TimeOfDayInterpolator<osg::Vec4f> mSunColor;

        // Fog depth/density
        Sky::TimeOfDayInterpolator<float> mLandFogDepth;

        // Color modulation for the sun itself during sunset
        osg::Vec4f mSunDiscSunsetColor;

        // Cloud animation speed multiplier
        float mCloudSpeed;

        // Value between 0 and 1, defines the strength of the sun glare effect.
        // Also appears to modify how visible the sun, moons, and stars are for various weather effects.
        float mGlareView;

        // Fog factor and offset used with distant land rendering.
        struct
        {
            float FogFactor;
            float FogOffset;
        } mDL;

        // Sound effects
        // This is used for Blight, Ashstorm and Blizzard (Bloodmoon)
        ESM::RefId mAmbientLoopSoundID;
        // This is used for Rain and Thunderstorm
        ESM::RefId mRainLoopSoundID;

        std::array<ESM::RefId, 4> mThunderSoundID;

        /// What this weather drops, how hard, and how fast the wind drives it.
        ///
        /// **Read once, by `Weather::downpourAt`, and never spelled out here.** The dozen keys
        /// behind it used to be read in this constructor and again in that function, which is how
        /// the wind came to reach the drops through a rule only one of the two applied.
        ///
        /// `mIsStorm` in it is also what makes a character shield their eyes and walk slower into
        /// it (`fStromWalkMult`), and what turns the clouds to come off Red Mountain.
        ::Weather::Downpour mDownpour;

        // Transition threshold to spawn rain
        float mRainThreshold;

        osg::Vec3f mStormDirection;

        float mCloudsMaximumPercent;

        // Note: For Weather Blight, there is a "Disease Chance" (=0.1) setting. But according to MWSFD this feature
        // is broken in the vanilla game and was disabled.

        float transitionDelta() const;
        float cloudBlendFactor(const float transitionRatio) const;

        float calculateThunder(const float transitionRatio, const float elapsedSeconds, const bool isPaused);

    private:
        float mTransitionDelta;

        // Note: In MW, only thunderstorms support these attributes, but in the interest of making weather more
        // flexible, these settings are imported for all weather types. Only thunderstorms will normally have any
        // non-zero values.
        float mThunderFrequency;
        float mThunderThreshold;
        float mFlashDecrement;

        float mFlashBrightness;

        void flashDecrement(const float elapsedSeconds);
        float thunderChance(const float transitionRatio, const float elapsedSeconds) const;
        void lightningAndThunder(void);
    };

    /// A class for storing a region's weather.
    class RegionWeather
    {
    public:
        explicit RegionWeather(const ESM::Region& region);
        explicit RegionWeather(const ESM::RegionWeatherState& state);

        operator ESM::RegionWeatherState() const;

        void setChances(std::span<const uint8_t> chances);
        std::span<const uint8_t> getChances() const;

        void setWeather(int weatherID);

        int getWeather();

    private:
        int mWeather;
        std::vector<uint8_t> mChances;

        void chooseNewWeather();
    };

    /// A class that acts as a model for the moons.
    /// One moon's place in the sky, as the renderer wants it stated.
    ///
    /// **The arithmetic is `Weather::MoonModel`'s and this is the adapter.** Two renderers need the
    /// same answers out of the `Moons_*` settings — this one and `openmw-rtxtool`, which has no
    /// weather system to ask — so the clock itself sits in a component below both, and what is left
    /// here is turning what it says into the type the sky was already being handed.
    class MoonModel
    {
    public:
        explicit MoonModel(const std::string& name)
            : mModel(name)
        {
        }

        MoonModel(float fadeInStart, float fadeInFinish, float fadeOutStart, float fadeOutFinish, float axisOffset,
            float speed, float dailyIncrement, float fadeStartAngle, float fadeEndAngle, float moonShadowEarlyFadeAngle)
            : mModel(fadeInStart, fadeInFinish, fadeOutStart, fadeOutFinish, axisOffset, speed, dailyIncrement,
                fadeStartAngle, fadeEndAngle, moonShadowEarlyFadeAngle)
        {
        }

        MWRender::MoonState calculateState(const TimeStamp& gameTime) const;

    private:
        Sky::MoonModel mModel;
    };

    /// Interface for weather settings
    class WeatherManager
    {
    public:
        // Have to pass fallback and Store, can't use singleton since World isn't fully constructed yet at the time
        WeatherManager(MWRender::RenderingManager& rendering, MWWorld::ESMStore& store);
        ~WeatherManager();

        /**
         * Change the weather in the specified region by id of the weather
         * @param region that should be changed
         * @param ID of the weather setting to shift to
         */
        void changeWeather(const ESM::RefId& regionID, const unsigned int weatherID);
        void changeWeather(const ESM::RefId& regionID, const ESM::RefId& weatherID);
        void modRegion(const ESM::RefId& regionID, std::span<const uint8_t> chances);
        std::span<const uint8_t> getRegionChances(const ESM::RefId& regionID) const;
        void playerTeleported(const ESM::RefId& playerRegion, bool isExterior);

        /**
         * Per-frame update
         * @param duration
         * @param paused
         */
        void update(float duration, bool paused, const TimeStamp& time, bool isExterior);

        void stopSounds();

        float getWindSpeed() const;
        NightDayMode getNightDayMode() const;

        /// Are we in an ash or blight storm?
        bool isInStorm() const;

        osg::Vec3f getStormDirection() const;

        void advanceTime(double hours, bool incremental);

        const std::vector<Weather>& getAllWeather() { return mWeatherSettings; }

        const Weather& getWeather() { return mWeatherSettings[mCurrentWeather]; }

        const Weather* getWeather(size_t index) const;

        const Weather* getWeather(const ESM::RefId& id) const;

        int getWeatherID() const { return mCurrentWeather; }

        const Weather* getNextWeather()
        {
            if (mNextWeather > -1)
                return &mWeatherSettings[mNextWeather];
            return nullptr;
        }

        int getNextWeatherID() const { return mNextWeather; }

        float getTransitionFactor() const { return mTransitionFactor; }

        bool useTorches(float hour) const;

        float getSunPercentage(float hour) const;

        float getSunVisibility() const;

        std::vector<Moon> getCurrentMoons(const TimeStamp& time) const;

        void write(ESM::ESMWriter& writer, Loading::Listener& progress);

        bool readRecord(ESM::ESMReader& reader, uint32_t type);

        void clear();

    private:
        MWWorld::ESMStore& mStore;
        MWRender::RenderingManager& mRendering;
        float mSunriseTime;
        float mSunsetTime;
        float mSunriseDuration;
        float mSunsetDuration;
        float mSunPreSunsetTime;

        Sky::TimeOfDaySettings mTimeSettings;

        // fading of night skydome
        Sky::TimeOfDayInterpolator<float> mNightFade;

        float mHoursBetweenWeatherChanges;
        float mRainSpeed;

        // underwater fog not really related to weather, but we handle it here because it's convenient
        Sky::TimeOfDayInterpolator<float> mUnderwaterFog;

        std::vector<Weather> mWeatherSettings;
        MoonModel mMasser;
        MoonModel mSecunda;

        float mWindSpeed;
        float mCurrentWindSpeed;
        float mNextWindSpeed;
        bool mIsStorm;
        bool mPrecipitation;
        osg::Vec3f mStormDirection;

        ESM::RefId mCurrentRegion;
        float mTimePassed;
        bool mFastForward;
        float mWeatherUpdateTime;
        float mTransitionFactor;
        NightDayMode mNightDayMode;
        int mCurrentWeather;
        int mNextWeather;
        int mQueuedWeather;
        std::map<ESM::RefId, RegionWeather> mRegions;
        MWRender::WeatherResult mResult;

        MWBase::Sound* mAmbientSound{ nullptr };
        ESM::RefId mPlayingAmbientSoundID;
        MWBase::Sound* mRainSound{ nullptr };
        ESM::RefId mPlayingRainSoundID;

        /// The model a storm drives past the eye comes from `Weather::stormEffect`, which is where
        /// that table lives now that a harness with no weather system reads it too.
        void addWeather(const std::string& name, float dlFactor, float dlOffset);

        void importRegions();

        void regionalWeatherChanged(const ESM::RefId& regionID, RegionWeather& region);
        bool updateWeatherTime();
        bool updateWeatherRegion(const ESM::RefId& playerRegion);
        void updateWeatherTransitions(const float elapsedRealSeconds);
        void forceWeather(const int weatherID);

        bool inTransition() const;
        void addWeatherTransition(const int weatherID);

        void calculateWeatherResult(const float gameHour, const float elapsedSeconds, const bool isPaused);
        void calculateResult(const int weatherID, const float gameHour);
        void calculateTransitionResult(const float factor, const float gameHour);
        float calculateWindSpeed(int weatherId, float currentSpeed);
    };
}

#endif // GAME_MWWORLD_WEATHER_H
