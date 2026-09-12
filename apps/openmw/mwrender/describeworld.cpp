// The frame's own record of the world, and the call that hands it to whichever renderer draws.
//
// **`RenderingManager`'s, defined apart from the rest of it.** Everything here is what this fork
// added to the class — the record `WorldState` is, where each part of it is written, and the one
// call a frame makes to describe itself — and none of it touches what upstream's file does. Kept
// out of that file so that it reads as upstream's with the seam edits and nothing else.
#include "renderingmanager.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include <osg/Camera>
#include <osg/FrameStamp>

#include <components/resource/resourcesystem.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/stateupdater.hpp>
#include <components/sky/sun.hpp>
#include <components/sky/timeofday.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/timestamp.hpp"

#include "camera.hpp"
#include "fogmanager.hpp"
#include "gl/sky.hpp"
#include "gl/water.hpp"
#include "renderer.hpp"
#include "sceneframe.hpp"
#include "stage.hpp"

namespace MWRender
{
    void RenderingManager::setWeather(const WeatherResult& weather)
    {
        mSky->setWeather(weather);

        // Kept apart rather than multiplied together: the alpha is how much of the sun is over the
        // horizon and the glare is how much of it this weather lets through, and only the first of
        // them says whether there is a sun to light anything at all.
        // **Everything `WorldState` says about the sky is taken from here**, off the weather the
        // world settled on, and nothing is read back out of the sky manager. It answers only when it
        // has been created, and it is created by whichever renderer is drawing — so a ray-traced
        // frame that asked it for the sky's colour got the black an unbuilt one starts at.
        mWorld.mSkyColour = weather.mSkyColor;
        mWorld.mCloudFog = weather.mFogColor;
        mWorld.mCloudDirection = weather.mStormDirection;
        mWorld.mNextCloudDirection = weather.mNextStormDirection;
        mWorld.mSunDiscColour = weather.mSunDiscColor;
        mWorld.mSunGlare = weather.mGlareView;
        // **The record and not the ramp**, for the same reason as the wind below: the depth the
        // weather blended is what a renderer whose fog is a medium reads, and `FogManager` is about
        // to make a start and an end of it.
        mWorld.mFogDepth = weather.mFogDepth;
        // **Nothing recorded is not a rate.** `Weather::transitionDelta` divides by
        // `Clouds_Maximum_Percent`, which the shipped fallbacks leave at nought for ash and blight,
        // so a transition into either hands over an infinity or a NaN. The rasterizer survives one —
        // a NaN opacity draws nothing and the old sky stays — and a tracer mixes its whole sky by
        // it. Nothing recorded means the deck has crossed at once.
        mWorld.mCloudBlend
            = std::isfinite(weather.mCloudBlendFactor) ? std::clamp(weather.mCloudBlendFactor, 0.f, 1.f) : 1.f;
        mWorld.mNightFade = weather.mNight ? weather.mNightFade : 0.f;

        // **The record and not the gust.** What this decides is how deep the fog's layer stands and
        // how fast its field is carried, and both are the weather's settled character rather than
        // the number the engine wanders about it. `WeatherResult::mWindSpeed` is the gust, and the
        // rasterizer's own uniform is what wants that one.
        mWorld.mBaseWindSpeed = weather.mBaseWindSpeed;
    }

    void RenderingManager::setMoonStates(const Sky::MoonState& masser, const Sky::MoonState& secunda)
    {
        mWorld.mMoons[0] = masser;
        mWorld.mMoons[1] = secunda;

        mSky->setMasserState(masser);
        mSky->setSecundaState(secunda);
    }

    EyeState RenderingManager::describeEye() const
    {
        return EyeState{
            .mNearClip = mNearClip,
            .mViewDistance = mViewDistance,
            .mProjectionMatrix = mPerViewUniformStateUpdater->getProjectionMatrix(),
            .mFieldOfView = mFieldOfViewOverridden ? mFieldOfViewOverride : mFieldOfView,
        };
    }

    WorldState RenderingManager::describeWorld() const
    {
        const MWBase::World& simulation = *MWBase::Environment::get().getWorld();
        const bool underwater = mWater->isUnderwater(mCamera->getPosition());

        // The simulation's "no transition" is -1, and `WorldState` would rather say it in the type.
        const int next = simulation.getNextWeatherScriptId();
        const std::optional<int> nextWeather = next < 0 ? std::nullopt : std::optional(next);

        // What the world settled on is already in `mWorld`, written where each part of it was
        // decided. What is left answers per frame, so no setter can have written it.
        WorldState described = mWorld;

        described.mSunColour = mSunLight->getDiffuse();
        described.mAmbientColour = mSunLight->getAmbient();
        described.mNightEye = mSunLight->getAmbient() - mAmbientColor;

        // **The sky manager's, and read rather than kept.** It exists under both renderers — only its
        // nodes are built lazily — so its clock and its particle systems are the one copy of each.
        described.mRain = mSky->getRainNode();
        described.mWeatherEffect = mSky->getParticleNode();
        described.mRainOnWater = mSky->getRainRipplesEnabled() ? mSky->getPrecipitationAlpha() : 0.f;
        described.mCloudScroll = mSky->getCloudAnimationTimer();
        described.mStarRoll = mSky->getAtmosphereNightRoll();

        described.mLocation = simulation.isCellExterior() ? Location::Exterior
            : simulation.isCellQuasiExterior()            ? Location::QuasiExterior
                                                          : Location::Interior;

        // **A room's facts, read off the cell the player stands in.** The weather system stops the
        // moment they step inside, so everything the setters above wrote for the sky is the last
        // outdoor hour's: the record the content wrote for the room, the depth of its fog, and a
        // sun that is all there — a renderer that scaled its sunlight by the share would light the
        // room with whatever fraction of a sunset it walked in on. The sun's position is the one
        // `configureAmbient` gave the light, which is where the rasterizer points it too.
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
            described.mFogDepth = mood.mFogDensity;
            described.mSunPosition = mSunLight->getPosition();
            described.mSunVector = -mSunLight->getPosition();
            described.mSunAtNight = false;
            described.mSunDiscColour = osg::Vec4f(1.f, 1.f, 1.f, 1.f);
            described.mSunGlare = 1.f;
        }

        described.mUnderwater = underwater;
        described.mFog = { mFog->getFogColor(underwater), mFog->getFogStart(underwater), mFog->getFogEnd(underwater) };
        described.mAir = { mFog->getFogColor(false), mFog->getFogStart(false), mFog->getFogEnd(false) };

        described.mGameHour = simulation.getTimeStamp().getHour();
        // **Off the hour rather than off the disc's alpha**, which the rasterizer leaves at one all
        // night with the disc hidden. `Sky::sunShareAt` is the one place the ramp is spelled.
        described.mSunShare = Sky::sunShareAt(described.mGameHour, Sky::TimeOfDaySettings::shared());
        described.mWeatherId = simulation.getCurrentWeatherScriptId();
        described.mNextWeatherId = nextWeather;
        described.mWeatherTransition = simulation.getWeatherTransition();
        described.mWindSpeed = simulation.getWindSpeed();

        return described;
    }

    void RenderingManager::renderFrame()
    {
        // **Where the eye is, told to the sky before the frame.** The cull traversal tells it the
        // same thing under the rasterizer; a renderer that culls nothing has to say it here, or the
        // underwater switch that freezes the rain reads the point the last cull left. Here and not
        // in `update`, because `Camera::updateCamera` writes the view matrix from the update
        // traversal, which runs between the two.
        mSky->setViewPoint(mStage.getCamera().getInverseViewMatrix().getTrans());

        const WorldState world = describeWorld();
        const EyeState seenFrom = describeEye();

        const SceneFrame frame{
            .mScene = *mSceneRoot,
            .mCamera = mStage.getCamera(),
            .mWhen = mStage.getFrameStamp(),
            .mWorld = world,
            .mEye = seenFrom,
            .mImages = *mResourceSystem->getImageManager(),
            .mTerrain = *mTerrain,
            .mObjectStorage = mObjectStorage,
        };

        mRenderer.renderFrame(frame);
    }
}
