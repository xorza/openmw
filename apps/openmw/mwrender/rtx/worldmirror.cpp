#include "worldmirror.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include <osg/Geometry>
#include <osg/Image>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/PositionAttitudeTransform>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/cellgrid.hpp>
#include <components/rtx/colour.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/residency.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/skylight.hpp>
#include <components/rtx/slot.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/sceneutil/waterutil.hpp>
#include <components/settings/values.hpp>
#include <components/sky/moonstate.hpp>
#include <components/sky/timeofday.hpp>
#include <components/terrain/world.hpp>
#include <components/vfs/pathutil.hpp>

#include "../../mwworld/cellstore.hpp"
#include "../sceneframe.hpp"
#include "../vismask.hpp"

namespace MWRender
{
    namespace
    {
        /// The game's own models and images, as `Rtx::CellReader` asks for them.
        class SceneContent final : public Rtx::ContentSource
        {
        public:
            explicit SceneContent(Resource::SceneManager& scenes)
                : mScenes(scenes)
            {
            }

            osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path) override
            {
                // Uncompiled, because nothing here has a context to compile for: `compile` queues
                // the model's OpenGL objects, and this renderer initialises no OpenGL.
                return mScenes.getTemplate(path, false);
            }

            osg::ref_ptr<const osg::Image> getImage(VFS::Path::NormalizedView path) override
            {
                return Rtx::openImage(*mScenes.getImageManager(), path);
            }

        private:
            Resource::SceneManager& mScenes;
        };

        /// What every walk this renderer makes takes: an exclusion of what the ray tracer draws
        /// itself, never a selection of what a walk is interested in. A node mask is AND-ed at
        /// every node, so naming `Mask_WeatherParticles` to mean "the weather subtree" extracted
        /// every storm with its particles missing, because a blizzard's own particles are marked
        /// `Mask_ParticleSystem`. Which subtree is walked is answered by where the walk starts.
        constexpr osg::Node::NodeMask sWorldTraversal
            = ~static_cast<osg::Node::NodeMask>(Mask_Sky | Mask_Sun | Mask_SimpleWater);

        /// What a walk of a loaded model may see: the world's mask without the player bit, which is
        /// stamped on nothing a content file holds. The cell ring is given this and never the
        /// world's, because a mask that moves makes `Rtx::CellRing::forget` read the ring from
        /// nothing, and the player bit moves while the camera settles.
        osg::Node::NodeMask templateTraversal()
        {
            return sWorldTraversal & ~NifOsg::Loader::getHiddenNodeMask();
        }

        /// What the world walk may see. `WorldMirror::setShowsPlayer` says why the player is a
        /// question.
        osg::Node::NodeMask worldTraversal(const bool showsPlayer)
        {
            const osg::Node::NodeMask player = showsPlayer ? 0 : static_cast<osg::Node::NodeMask>(Mask_Player);

            return templateTraversal() & ~player;
        }
    }

    WorldMirror::WorldMirror()
        : mExtractor(mScene, &mTraversals)
        , mReach(Rtx::distantLandReach(Settings::rtx().mDistantLandCells, Settings::camera().mViewingDistance))
    {
        mRing.setStaticsEnabled(Settings::terrain().mObjectPaging);
        mRing.setMinSize(Settings::terrain().mObjectPagingMinSize);
        // The sky is not mirrored: the engine rebuilds it every frame, state sets and all, so
        // walking it churns the identity maps and makes every frame a full rebuild, and a ray that
        // reaches the sky gets this renderer's own. The simple water is the local map's copy of the
        // sea, which a mirror walking both would place twice. What the content hides is asked of the
        // loader, whose hidden bit is one bit rather than none so the update traversal still reaches
        // a hidden bone.
        mExtractor.setTraversalMask(worldTraversal(mShowsPlayer));

        // What is left of the two is the sea, which this renderer stands: upstream's plane, as
        // `MWRender::Water` makes it, on a transform a frame moves.
        mExtractor.setWaterMask(Mask_Water);

        osg::ref_ptr<osg::Geometry> sea = SceneUtil::createWaterGeometry(Constants::CellSizeInUnits * 150, 40, 900);
        sea->setNodeMask(Mask_Water);
        sea->setName("Sea Geometry");
        mSea = new osg::PositionAttitudeTransform;
        mSea->setName("Sea Root");
        mSea->addChild(sea);

        // The roots the game marks, so a camera's cull mask can keep or leave out what stands
        // under them — `rayMaskOf` is the other half.
        mExtractor.setClassMask(Rtx::InstanceClass::Actor, Mask_Actor | Mask_Player);
        mExtractor.setClassMask(Rtx::InstanceClass::Effect, Mask_Effect);
        mExtractor.setClassMask(Rtx::InstanceClass::FirstPerson, Mask_FirstPerson);
    }

    void WorldMirror::attach(Resource::ResourceSystem& resources)
    {
        mResources = &resources;
        mContent = std::make_unique<SceneContent>(*resources.getSceneManager());
    }

    void WorldMirror::detach()
    {
        // **The ring's thread reads the storages the world owns.** A world with nothing in it is
        // what stops the thread and drops what it held.
        mRing.follow(Rtx::WorldAround{});

        mContent.reset();
        mResources = nullptr;
    }

    void WorldMirror::standSea(const MWWorld::CellStore& cell)
    {
        if (!cell.getCell()->isExterior())
        {
            mSeaCentre = osg::Vec2f(0.f, 0.f);
            return;
        }

        constexpr int half = Constants::CellSizeInUnits / 2;
        const int x = cell.getCell()->getGridX() * Constants::CellSizeInUnits + half;
        const int y = cell.getCell()->getGridY() * Constants::CellSizeInUnits + half;
        mSeaCentre = osg::Vec2f(static_cast<float>(x), static_cast<float>(y));
    }

    void WorldMirror::setShowsPlayer(const bool shows)
    {
        if (shows == mShowsPlayer)
            return;

        mShowsPlayer = shows;
        mExtractor.setTraversalMask(worldTraversal(mShowsPlayer));
    }

    Rtx::ExtractionStats WorldMirror::mirror(const SceneFrame& frame, const std::size_t frameNumber)
    {
        // The world's clock and not this renderer's, or the controllers would run while the game
        // was paused; the emitters by the gap between frames, which the extractor clamps, because
        // they integrate it rather than read the hour.
        mExtractor.setSimulationTime(frame.mWhen.getSimulationTime());
        mExtractor.advanceEmitters(frame.mWhen.getSimulationTime() - mLastSimulationTime);
        mLastSimulationTime = frame.mWhen.getSimulationTime();

        // What goes is the lists a walk refills wholesale; the meshes, materials and textures stay
        // because the structures were built from them, and the placements because they are
        // addressed by slot.
        mScene.clearPlacement();

        // The moons' portraits, once: a moon is drawn by a ray that reached nothing, so no material
        // speaks for the slot and the sweep would take it on the first frame a cell died.
        if (mMoonFaces.mMasser == Rtx::sNoIndex)
        {
            mMoonFaces = Rtx::addMoonFaces(mScene);
            mSkyContent = Rtx::addSkyContent(mScene, *mResources->getSceneManager(),
                Rtx::SkyMeshes{ .mClouds = Settings::models().mSkyclouds,
                    .mStars = Settings::models().mSkynight02,
                    .mStarsFallback = Settings::models().mSkynight01 });
        }

        // What the weather drops, walked as a second root, because the sky's mask keeps the world
        // walk out of that subtree: the same systems the rasterizer draws, stood at the eye.
        const osg::Vec3f eye = frame.mCamera.getInverseViewMatrix().getTrans();
        mEye = eye;
        Rtx::mirrorPrecipitation(mExtractor, frame.mWorld.mRain, eye, frame.mWorld.mUnderwater, frameNumber);
        Rtx::mirrorPrecipitation(mExtractor, frame.mWorld.mWeatherEffect, eye, frame.mWorld.mUnderwater, frameNumber);

        // The sea, where the frame says there is one: hidden by its mask otherwise, as the
        // rasterizer's `updateVisible` hid the same plane, so the walk leaves no placement of it.
        mSea->setPosition(osg::Vec3f(mSeaCentre.x(), mSeaCentre.y(), frame.mWorld.mWaterHeight));
        mSea->setNodeMask(frame.mWorld.mWaterEnabled ? ~0u : 0u);
        mExtractor.extract(*mSea, osg::Matrixf::identity(), 0, frameNumber);

        // The eye, the reach, the world's own grid and the hour, said once to the ring: what the
        // game has stood for itself is what the ring may not stand again, and its lamps burn at
        // the world's clock as the graph's do.
        const Rtx::WorldAround around{
            .mWorld = {
                .mStorage = &frame.mObjectStorage,
                .mGround = frame.mTerrain.getStorage(),
                .mContent = mContent.get(),
                .mWorldspace = frame.mTerrain.getWorldspace(),
                .mMask = templateTraversal(),
            },
            .mEye = eye,
            .mReach = mReach,
            .mActiveGrid = frame.mTerrain.getActiveGrid(),
            .mExterior = !frame.mWorld.isInteriorCell(),
            .mSimulationTime = frame.mWhen.getSimulationTime(),
        };

        mRing.setFrame(frameNumber);

        // Told once a frame, because what the graph does not hold is the frame's to say. Every
        // world walk asks it from here, and the precipitation walk above cannot: it is a subtree.
        mRing.follow(around);
        mExtractor.follow(&mRing);

        // One walk over the whole graph, where every path is already distinct.
        return mExtractor.extractWorld(frame.mScene, osg::Matrixf::identity(), 0, frameNumber);
    }

    Rtx::SceneUpload WorldMirror::hand(Rtx::Renderer& renderer, Resource::ImageManager& images, Rtx::FrameSpend& spend)
    {
        return mUploader.hand(renderer,
            Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(),
                .mScene = mScene,
                .mImages = images,
                .mComposites = &mComposites,
                .mReadings = &mRing.getHolds(),
                .mSpend = &spend });
    }

    void WorldMirror::settle()
    {
        // After the frame and not before the walk, so this frame does not measure its motion
        // against itself; on the frames the trace refused as well, because the walk still ran.
        mExtractor.advance();

        // What the walk did not find has gone. The graph is the whole world every frame, which is
        // what makes mark and sweep sound; the identity maps hold their keys alive until it runs.
        // Last, because it bumps the epoch the next walk is measured against.
        if (const Rtx::Retirement went = mExtractor.retire(); !went.empty())
            Log(Debug::Info) << "Ray tracing dropped " << went.mMeshes << " meshes and " << went.mMaterials
                             << " materials the world no longer has";
    }

    Rtx::WorldReading WorldMirror::readWorld(const WorldState& world, const float seconds) const
    {
        // Where the sun is, and the light comes back along it. `mSunVector` is where the
        // rasterizer's light travels and is not the negation of this; nothing that traces can hold both.
        osg::Vec3f discAt(world.mSky.mSunPosition.x(), world.mSky.mSunPosition.y(), world.mSky.mSunPosition.z());
        if (discAt.length2() > 0.0f)
            discAt.normalize();

        // A room's light out of the record the cell wrote and not the rasterizer's reading of it,
        // which lifts the ambient for its own falloff and points a directional light.
        const std::optional<Rtx::Daylight> room = world.mRoom.has_value()
            ? std::optional(Rtx::makeRoomLight(
                *world.mRoom, osg::Vec3f(world.mNightEye.x(), world.mNightEye.y(), world.mNightEye.z())))
            : std::nullopt;

        // The horizon is the fog and the zenith is the sky's own, which is the pair Morrowind
        // records. Decoded here, because the world does not know what a transport is.
        const osg::Vec3f haze = room.has_value() ? room->mSkyHorizon : Rtx::decodeColour(world.mAir.mColour);

        // An interior has no sky colour: the weather system stops writing it indoors, so the air's
        // own colour stands in. A quasi-exterior has weather and so has one.
        const osg::Vec3f zenith = room.has_value() ? room->mSkyZenith : Rtx::decodeColour(world.mSky.mSkyColour);

        // The sun is not assembled here: everything the world says about it goes to the one builder
        // that decides what a sun may be, and the light is taken whole from whichever built it.
        const Sky::TimeOfDaySettings& times = Sky::TimeOfDaySettings::shared();
        const Rtx::SkyReading reading{
            .mSunPosition = discAt,

            // Off the hour rather than off the disc's alpha, which the rasterizer leaves at one all
            // night with the disc hidden.
            .mSunShare = Rtx::sunShareAt(world.mGameHour, times),
            .mSunShareAloft = Rtx::sunShareAloft(world.mGameHour, times),
            .mSunColour = Rtx::decodeColour(world.mSunColour),
            .mAmbient = Rtx::decodeColour(world.mAmbientColour),
            .mDiscColour = Rtx::decodeColour(world.mSky.mSunDiscColour),
            .mGlare = world.mSky.mSunGlare,
        };
        const Rtx::Skylight light = room.has_value() ? room->mLight : Rtx::makeSkylight(reading);

        // The recorded depth and not the ramp `FogManager` made of it, which exists to hide a far
        // clip plane. A quasi-exterior stands in the weather's air, because the weather system is
        // run for one and the Construction Set greys its `AMBI` out; handed to `roomFog` it closed
        // over the sky. The two open-air builders differ only in the ring they close over.
        const auto openAir = world.mLocation == Location::Exterior ? &Rtx::exteriorFog : &Rtx::quasiExteriorFog;
        const Rtx::Fog air
            = room.has_value() ? room->mFog : openAir(haze, world.mSky.mFogDepth, world.mSky.mBaseWindSpeed, mReach);

        // Before the frame rather than into it, because the deck is lit by them (`Rtx::deckLight`).
        std::array<Rtx::MoonPlacement, 2> moons{};
        for (std::size_t moon = 0; moon < moons.size(); ++moon)
        {
            const Sky::MoonState& state = world.mSky.mMoons[moon];

            // The glare is applied here, where the rasterizer applies it too
            // (`SkyManager::setWeather` calls `Moon::adjustTransparency` after the hand-over).
            moons[moon] = Rtx::placeMoon(static_cast<Rtx::Moon>(moon), state.mRotationFromHorizon,
                state.mRotationFromNorth, state.mPhase, state.mDaylightFade * world.mSky.mSunGlare);
            moons[moon].mFace = mMoonFaces.of(static_cast<Rtx::Moon>(moon));
        }

        const auto weatherId = static_cast<std::uint32_t>(world.mWeatherId);

        return Rtx::WorldReading{
            .mDaylight = Rtx::Daylight{
                .mLight = light,
                .mSkyHorizon = haze,
                .mSkyZenith = zenith,
                .mStarFade = world.mSky.mNightFade,
                .mFog = air,
            },
            .mOutdoors = world.isOutdoors(),
            .mGlare = world.mSky.mSunGlare,
            .mStarRoll = world.mSky.mStarRoll,
            .mSky = mSkyContent,
            .mMoons = moons,
            .mClouds = Rtx::CloudCrossing{
                .mWeather = weatherId,
                // The current weather twice where nothing is arriving, since the deck crosses
                // unconditionally: naming it on both sides at a blend of nothing is what lets it.
                .mNext = world.mNextWeatherId.has_value() ? static_cast<std::uint32_t>(*world.mNextWeatherId)
                                                          : weatherId,
                .mBlend = world.mSky.mCloudBlend,
                .mDirection = world.mSky.mCloudDirection,
                .mNextDirection = world.mSky.mNextCloudDirection,
                .mScroll = world.mSky.mSkyCloudScroll,
            },

            // Negative infinity and not zero: zero is sea level, and a cell with no water has to
            // answer "how deep is this point" with never.
            .mWaterLevel = world.mWaterEnabled ? world.mWaterHeight : -std::numeric_limits<float>::infinity(),

            // What the sea is animated by, in elapsed seconds rather than frames, or the sea would
            // slow down whenever the frame did.
            .mSeconds = seconds,
            .mSkySeconds = world.mSky.mSkySeconds,
            .mRainOnWater = world.mRainOnWater,
        };
    }
}
