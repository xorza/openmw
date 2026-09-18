// The pair of `sceneframe.hpp`: the `RenderingManager` members that fill its records and hand the
// frame to whichever renderer draws.
//
// **`RenderingManager`'s, defined apart from the rest of it.** Everything here is what this fork
// added to the class — how `WorldState` and `EyeState` are read off the world, and the one call a
// frame makes to describe itself — and none of it touches what upstream's file does. Kept out of
// that file so that it reads as upstream's with the seam edits and nothing else.
#include "renderingmanager.hpp"

#include <cassert>
#include <optional>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/PositionAttitudeTransform>

#include <components/sceneutil/lightmanager.hpp>
#include <components/sky/sundisc.hpp>

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
#include "skystate.hpp"

namespace MWRender
{
    osg::Vec4f sunDiscOf(const SkyState& sky, const WorldState& world)
    {
        return sky.mOutdoors ? osg::Vec4f(Sky::sunDiscPosition(sky.mSunDirection), 0.f) : world.mSunLightPosition;
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
        // Not `const`: `getTimeManager` is not.
        MWBase::World& simulation = *MWBase::Environment::get().getWorld();
        const bool underwater = isUnderwater(mCamera->getPosition());

        // The simulation's "no transition" is -1, and `WorldState` would rather say it in the type.
        const int next = simulation.getNextWeatherScriptId();
        const std::optional<int> nextWeather = next < 0 ? std::nullopt : std::optional(next);

        // **Off the light and the toggles, and nothing recorded twice.** The sun light is the one
        // copy of what the four setters wrote, and what the weather settled is the weather
        // manager's, handed beside this.
        WorldState described;
        described.mSunLightPosition = mSunLight->getPosition();
        described.mSunColour = mSunLight->getDiffuse();
        described.mAmbientColour = mSunLight->getAmbient();
        described.mNightEye = mSunLight->getAmbient() - mAmbientColor;
        described.mSunVisibility = mSunVisibility;
        described.mSkyShown = mSkyEnabled;
        described.mMoonRed = mMoonRed;

        described.mLocation = simulation.isCellExterior() ? Location::Exterior
            : simulation.isCellQuasiExterior()            ? Location::QuasiExterior
                                                          : Location::Interior;

        // **A room's facts, read off the cell the player stands in.** The weather system stops the
        // moment they step inside, so everything it settled for the sky is the last outdoor
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
        described.mTimeScale = simulation.getTimeManager()->getGameTimeScale();
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
            .mSky = MWBase::Environment::get().getWorld()->getSkyState(),
            .mPrecipitation = *mPrecipitation,
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
        // **Where the eye is, told to the precipitation before the draw**, so the underwater
        // switch that freezes the rain reads this frame's eye and not the point a traversal last
        // left. Here and not in `describeFrame`, because `Camera::updateCamera` writes the view
        // matrix from the update traversal, which runs between the two.
        const osg::Camera& camera = mRenderer.getCamera();
        mPrecipitation->setViewPoint(camera.getInverseViewMatrix().getTrans());

        assert(mFrame.has_value() && "a frame is described before it is drawn");
        mRenderer.renderFrame(*mFrame);
    }
}
