#include "framedescriber.hpp"

#include <cassert>
#include <optional>

#include <osg/Camera>
#include <osg/FrameStamp>

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
#include "renderingmanager.hpp"
#include "skystate.hpp"

namespace MWRender
{
    osg::Vec4f sunDiscOf(const SkyState& sky, const WorldState& world)
    {
        return sky.mOutdoors ? osg::Vec4f(Sky::sunDiscPosition(sky.mSunDirection), 0.f) : world.mSunLightPosition;
    }

    WorldState FrameDescriber::describeWorld(const FrameSources& sources) const
    {
        // Not `const`: `getTimeManager` is not.
        MWBase::World& simulation = *MWBase::Environment::get().getWorld();

        // The simulation's "no transition" is -1, and `WorldState` would rather say it in the type.
        const int next = simulation.getNextWeatherScriptId();
        const std::optional<int> nextWeather = next < 0 ? std::nullopt : std::optional(next);

        // **Off the light and the toggles, and nothing recorded twice.** The sun light is the one
        // copy of what the four setters wrote, and what the weather settled is the weather
        // manager's, handed beside this.
        WorldState described;
        described.mSunLightPosition = sources.mSun.getPosition();
        described.mSunColour = sources.mSun.getDiffuse();
        described.mAmbientColour = sources.mSun.getAmbient();
        described.mNightEye = sources.mSun.getAmbient() - sources.mAmbientBeforeNightEye;
        described.mSunVisibility = mSunVisibility;
        described.mSkyShown = mSkyShown;
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

        described.mWater = mWater;
        described.mUnderwater = mWater.isUnderwater(sources.mEyePosition);
        described.mAir
            = { sources.mFog.getFogColor(false), sources.mFog.getFogStart(false), sources.mFog.getFogEnd(false) };
        described.mWaterFog
            = { sources.mFog.getFogColor(true), sources.mFog.getFogStart(true), sources.mFog.getFogEnd(true) };
        described.mPlayerPosition = player.getRefData().getPosition().asVec3();

        described.mGameHour = simulation.getTimeStamp().getHour();
        described.mTimeScale = simulation.getTimeManager()->getGameTimeScale();
        described.mWeatherId = simulation.getCurrentWeatherScriptId();
        described.mNextWeatherId = nextWeather;
        described.mWeatherTransition = simulation.getWeatherTransition();
        described.mWindSpeed = simulation.getWindSpeed();

        return described;
    }

    const SceneFrame& FrameDescriber::describe(const FrameSources& sources)
    {
        mWorld = describeWorld(sources);
        mEye = sources.mEye;

        mFrame.emplace(SceneFrame{
            .mScene = sources.mScene,
            .mWhen = sources.mWhen,
            .mSky = MWBase::Environment::get().getWorld()->getSkyState(),
            .mPrecipitation = sources.mPrecipitation,
            .mWorld = mWorld,
            .mEye = mEye,
            .mTerrain = sources.mTerrain,
            .mObjectStorage = sources.mObjectStorage,
            .mDeltaTime = mDeltaTime,
            .mPaused = mPaused,
        });

        return *mFrame;
    }

    const SceneFrame& FrameDescriber::get() const
    {
        assert(mFrame.has_value() && "a frame is described before it is drawn");
        return *mFrame;
    }

    // **`RenderingManager`'s three frame members, defined here and not in its own file**, so that
    // file reads as upstream's with the seam edits and nothing else: everything below is what this
    // fork added to the class, and all of it is about the describer above.

    EyeState RenderingManager::describeEye() const
    {
        return EyeState{
            .mNearClip = mNearClip,
            .mViewDistance = mViewDistance,
            .mProjectionMatrix = mFrame.getProjection(),
            .mFieldOfView = mFieldOfViewOverridden ? mFieldOfViewOverride : mFieldOfView,
            .mArmsFieldOfView = mFirstPersonFieldOfView,
            .mPlayersEye = mCamera->getMode() != Camera::Mode::Static,
        };
    }

    void RenderingManager::describeFrame()
    {
        mRenderer.describeFrame(mFrame.describe(FrameSources{
            .mScene = *mSceneRoot,
            .mWhen = mRenderer.getFrameStamp(),
            .mSun = *mSunLight,
            .mAmbientBeforeNightEye = mAmbientColor,
            .mFog = *mFog,
            .mEyePosition = mCamera->getPosition(),
            .mPrecipitation = *mPrecipitation,
            .mTerrain = *mTerrain,
            .mObjectStorage = mObjectStorage,
            .mEye = describeEye(),
        }));
    }

    void RenderingManager::renderFrame()
    {
        // **Where the eye is, told to the precipitation before the draw**, so the underwater
        // switch that freezes the rain reads this frame's eye and not the point a traversal last
        // left. Here and not in `describeFrame`, because `Camera::updateCamera` writes the view
        // matrix from the update traversal, which runs between the two.
        const osg::Camera& camera = mRenderer.getCamera();
        mPrecipitation->setViewPoint(camera.getInverseViewMatrix().getTrans());

        mRenderer.renderFrame(mFrame.get());
    }
}
