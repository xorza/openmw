// The pair of `sceneframe.hpp`: the `RenderingManager` members that fill its records and hand the
// frame to whichever renderer draws.
//
// **`RenderingManager`'s, defined apart from the rest of it.** Everything here is what this fork
// added to the class — where each part of `SkySettled` is written, how `WorldState` and `EyeState`
// are read off the world, and the one call a frame makes to describe itself — and none of it
// touches what upstream's file does. Kept out of that file so that it reads as upstream's with the
// seam edits and nothing else.
#include "renderingmanager.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <optional>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Math>
#include <osg/PositionAttitudeTransform>

#include <components/sceneutil/lightmanager.hpp>
#include <components/sky/skyclock.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/timestamp.hpp"

#include "camera.hpp"
#include "fogmanager.hpp"
#include "precipitation.hpp"
#include "renderer.hpp"
#include "sceneframe.hpp"

namespace MWRender
{
    namespace
    {
        /// The deck's sheet repeats every four texture units, which is where a scroll wraps.
        float scrolled(const float scroll, const float delta)
        {
            const float advanced = scroll + delta;
            return advanced >= 4.f ? advanced - 4.f : advanced;
        }
    }

    void RenderingManager::setWeather(const WeatherResult& weather)
    {
        mPrecipitation->setWeather(weather);

        // Whole, for the dome: the rasterizer's sky manager reads the same record upstream handed
        // it, off the frame instead.
        mWeather = weather;
        mSky.mWeather = &mWeather;

        // Kept apart rather than multiplied together: the alpha is how much of the sun is over the
        // horizon and the glare is how much of it this weather lets through, and only the first of
        // them says whether there is a sun to light anything at all.
        // **Everything `SkySettled` says about the weather is taken from here**, off the weather
        // the world settled on, and nothing is read back out of a dome: the rasterizer's sky
        // manager is its own, built lazily, and answered black before it was built.
        mSky.mSkyColour = weather.mSkyColor;
        mSky.mCloudFog = weather.mFogColor;
        mSky.mCloudDirection = weather.mStormDirection;
        mSky.mNextCloudDirection = weather.mNextStormDirection;
        mSky.mSunDiscColour = weather.mSunDiscColor;
        mSky.mSunGlare = weather.mGlareView;
        // **The record and not the ramp**, for the same reason as the wind below: the depth the
        // weather blended is what a renderer whose fog is a medium reads, and `FogManager` is about
        // to make a start and an end of it.
        mSky.mFogDepth = weather.mFogDepth;
        // **Nothing recorded is not a rate.** `Weather::transitionDelta` divides by
        // `Clouds_Maximum_Percent`, which the shipped fallbacks leave at nought for ash and blight,
        // so a transition into either hands over an infinity or a NaN. The rasterizer survives one —
        // a NaN opacity draws nothing and the old sky stays — and a tracer mixes its whole sky by
        // it. Nothing recorded means the deck has crossed at once.
        mSky.mCloudBlend
            = std::isfinite(weather.mCloudBlendFactor) ? std::clamp(weather.mCloudBlendFactor, 0.f, 1.f) : 1.f;
        mSky.mNightFade = weather.mNight ? weather.mNightFade : 0.f;

        // **The record and not the gust.** What this decides is how deep the fog's layer stands and
        // how fast its field is carried, and both are the weather's settled character rather than
        // the number the engine wanders about it. `WeatherResult::mWindSpeed` is the gust, and the
        // rasterizer's own uniform is what wants that one.
        mSky.mBaseWindSpeed = weather.mBaseWindSpeed;
    }

    void RenderingManager::setMoonStates(const Sky::MoonState& masser, const Sky::MoonState& secunda)
    {
        mSky.mMoons[0] = masser;
        mSky.mMoons[1] = secunda;
    }

    void RenderingManager::updateSkyClocks(float dt)
    {
        if (!mSky.mSkyEnabled)
            return;

        const float timeScale = MWBase::Environment::get().getWorld()->getTimeManager()->getGameTimeScale();

        // UV Scroll the clouds
        float cloudDelta = dt * mWeather.mCloudSpeed / 400.f;
        if (mTimescaleClouds)
            cloudDelta *= timeScale / 60.f;

        mSky.mCloudScroll = scrolled(mSky.mCloudScroll, cloudDelta);

        // rotate the stars by 360 degrees every 4 days
        mSky.mStarRoll += timeScale * dt * osg::DegreesToRadians(360.f) / (3600 * 96.f);

        // The same scroll on the sky's clock: the deck's speed is a rate over real seconds, which
        // the sky's clock is at the shipped `timescale`.
        const float skyDt = Sky::skyStep(dt, timeScale);
        mSky.mSkySeconds += skyDt;
        mSky.mSkyCloudScroll = scrolled(mSky.mSkyCloudScroll, skyDt * mWeather.mCloudSpeed / 400.f);
    }

    EyeState RenderingManager::describeEye() const
    {
        return EyeState{
            .mNearClip = mNearClip,
            .mViewDistance = mViewDistance,
            .mProjectionMatrix = mProjectionMatrix,
            .mFieldOfView = mFieldOfViewOverridden ? mFieldOfViewOverride : mFieldOfView,
            .mArmsFieldOfView = mFirstPersonFieldOfView,
            .mPlayersEye = mCamera->getMode() != Camera::Mode::Static,
        };
    }

    WorldState RenderingManager::describeWorld() const
    {
        const MWBase::World& simulation = *MWBase::Environment::get().getWorld();
        const bool underwater = isUnderwater(mCamera->getPosition());

        // The simulation's "no transition" is -1, and `WorldState` would rather say it in the type.
        const int next = simulation.getNextWeatherScriptId();
        const std::optional<int> nextWeather = next < 0 ? std::nullopt : std::optional(next);

        // What the world settled on about the sky is `mSky`, written where each part of it was
        // decided. What is left answers per frame, so no setter can have written it.
        WorldState described;
        described.mSky = mSky;
        described.mSunColour = mSunLight->getDiffuse();
        described.mAmbientColour = mSunLight->getAmbient();
        described.mNightEye = mSunLight->getAmbient() - mAmbientColor;

        // **The precipitation's, and read rather than kept.** Its particle systems are the one copy
        // both renderers walk.
        described.mRain = mPrecipitation->getRainNode();
        described.mWeatherEffect = mPrecipitation->getParticleNode();
        described.mRainOnWater
            = mPrecipitation->getRainRipplesEnabled() ? mPrecipitation->getPrecipitationAlpha() : 0.f;
        described.mPrecipitating = mPrecipitation->isOccluded();
        described.mPrecipitationRange = mPrecipitation->getOcclusionRange();

        described.mLocation = simulation.isCellExterior() ? Location::Exterior
            : simulation.isCellQuasiExterior()            ? Location::QuasiExterior
                                                          : Location::Interior;

        // **A room's facts, read off the cell the player stands in.** The weather system stops the
        // moment they step inside, so everything the setters wrote for the sky is the last outdoor
        // hour's, bar the sun `configureAmbient` pointed. The ray tracer lights a room from the
        // record alone.
        const MWWorld::Ptr& player = MWMechanics::getPlayer();
        if (described.mLocation == Location::Interior && player.isInCell())
        {
            const auto& mood = player.getCell()->getCell()->getMood();
            described.mRoom = ESM::Cell::AMBIstruct{
                .mAmbient = mood.mAmbiantColor,
                .mSunlight = mood.mDirectionalColor,
                .mFog = mood.mFogColor,
                .mFogDensity = mood.mFogDensity,
            };
        }

        described.mUnderwater = underwater;
        described.mWaterEnabled = mWaterEnabled && mWaterToggled;
        described.mWaterHeight = mWaterHeight;
        described.mAir = { mFog->getFogColor(false), mFog->getFogStart(false), mFog->getFogEnd(false) };
        described.mWaterFog = { mFog->getFogColor(true), mFog->getFogStart(true), mFog->getFogEnd(true) };
        described.mPlayerPosition = player.getRefData().getPosition().asVec3();

        described.mGameHour = simulation.getTimeStamp().getHour();
        described.mWeatherId = simulation.getCurrentWeatherScriptId();
        described.mNextWeatherId = nextWeather;
        described.mWeatherTransition = simulation.getWeatherTransition();
        described.mWindSpeed = simulation.getWindSpeed();

        return described;
    }

    void RenderingManager::describeFrame()
    {
        mFrameWorld = describeWorld();
        mFrameEye = describeEye();

        mFrame.emplace(SceneFrame{
            .mScene = *mSceneRoot,
            .mWhen = mRenderer.getFrameStamp(),
            .mWorld = mFrameWorld,
            .mEye = mFrameEye,
            .mTerrain = *mTerrain,
            .mObjectStorage = mObjectStorage,
            .mDeltaTime = mFrameDelta,
            .mPaused = mFramePaused,
        });

        mRenderer.describeFrame(*mFrame);
    }

    void RenderingManager::renderFrame()
    {
        // **Where the eye is, settled into the frame and told to the precipitation before the
        // draw**, so the trace and the underwater switch that freezes the rain both read this
        // frame's eye and not the point a traversal last left. Here and not in `describeFrame`,
        // because `Camera::updateCamera` writes the view matrix from the update traversal, which
        // runs between the two.
        const osg::Camera& camera = mRenderer.getCamera();
        mFrameEye.mView = camera.getViewMatrix();
        mPrecipitation->setViewPoint(camera.getInverseViewMatrix().getTrans());

        assert(mFrame.has_value() && "a frame is described before it is drawn");
        mRenderer.renderFrame(*mFrame);
    }
}
