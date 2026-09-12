#include "worldmirror.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/colour.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/renderer.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/settings/values.hpp>
#include <components/sky/timeofday.hpp>
#include <components/terrain/world.hpp>

#include "../sceneframe.hpp"

namespace MWRender
{
    namespace
    {
        /// What every walk this renderer makes takes.
        ///
        /// **An exclusion of what the ray tracer draws itself**, and never a selection of what a
        /// walk is interested in. A node mask is AND-ed at every node on the way down, so the bits
        /// have to be read as "which categories may be seen at all" — which is a different question
        /// from "which subtree am I walking", and that one is answered by where the walk starts.
        ///
        /// Conflating the two is a silent, total failure: naming `SceneUtil::Mask_WeatherParticles` here to
        /// mean "the weather subtree" extracted every storm in the game with all of its particles
        /// missing, because `Resource::SceneManager` marks a `ParticleSystem` drawable
        /// `SceneUtil::Mask_ParticleSystem` and a blizzard's own particles are not categorised as weather.
        ///
        /// The sky, the sun and the simple water are what this renderer draws for itself. What the
        /// content says is not there is a second exclusion, and it is asked of the loader that
        /// stamped it rather than named again here — see where this is installed.
        constexpr osg::Node::NodeMask sWorldTraversal = ~static_cast<osg::Node::NodeMask>(
            SceneUtil::Mask_Sky | SceneUtil::Mask_Sun | SceneUtil::Mask_SimpleWater);

        /// What a walk of a loaded model may see.
        ///
        /// **The world's mask without the one question the camera answers.** `SceneUtil::Mask_Player`
        /// is stamped on the player's own node in the world's graph and on nothing a content file
        /// holds, so it says nothing about a model a cell names.
        ///
        /// **Which is why the cell ring is given this and never the world's.** The mask is part of
        /// the world `Rtx::CellSupply` reads, so one that moves makes `Rtx::CellRing::forget` discard
        /// every held cell and read the ring from nothing. Measured on `one-cell-walk`, a camera
        /// settling between first and third person moved the bit twice while the world came up, and
        /// 121 cells were read three times and 85 grounds flattened three times for it.
        osg::Node::NodeMask templateTraversal()
        {
            return sWorldTraversal & ~NifOsg::Loader::getHiddenNodeMask();
        }

        /// What the world walk may see. `WorldMirror::setShowsPlayer` says why the player is a
        /// question.
        osg::Node::NodeMask worldTraversal(const bool showsPlayer)
        {
            const osg::Node::NodeMask player
                = showsPlayer ? 0 : static_cast<osg::Node::NodeMask>(SceneUtil::Mask_Player);

            return templateTraversal() & ~player;
        }
    }

    WorldMirror::WorldMirror()
        : mExtractor(mScene, &mTraversals)
        , mReach(Rtx::distantLandReach(Settings::rtx().mDistantLandCells, Settings::camera().mViewingDistance))
    {
        mRing.setStaticsEnabled(Settings::terrain().mObjectPaging);
        mRing.setMinSize(Settings::terrain().mObjectPagingMinSize);
        // **The sky is not mirrored.** It is the one subtree the engine rebuilds every frame —
        // state sets and all — so walking it churns the identity maps, and a sweep that drops four
        // materials a frame bumps the revision and makes every frame a full rebuild. Nothing is
        // lost by leaving it out: a ray that reaches the sky has missed everything, and what it
        // gets then is this renderer's own sky rather than the dome the rasterizer draws.
        //
        // **And `SceneUtil::Mask_SimpleWater` with them, which is a duplicate rather than a subtree to skip.**
        // `MWRender::Water` hangs two coplanar quads under one node — the world's water under
        // `SceneUtil::Mask_Water`, and a deep copy of it under `SceneUtil::Mask_SimpleWater` that exists for the local
        // map — and the rasterizer picks between them with the drawing camera's traversal mask.
        // A mirror that walks both places the sea twice, at the same height, as two meshes.
        //
        // **And what the content hides, which the loader is asked for rather than named twice.**
        // `RenderingManager` installs `SceneUtil::Mask_UpdateVisitor` as the hidden node mask, and a
        // `NifOsg::VisController` animating visibility swaps a node between it and every bit. It is
        // one bit rather than no bits at all so that the update traversal still reaches a hidden
        // bone to animate it — which is why a walk that ignores it traces what nothing draws.
        mExtractor.setTraversalMask(worldTraversal(mShowsPlayer));

        // What is left of the two is the world's own water, and it is the sea.
        mExtractor.setWaterMask(SceneUtil::Mask_Water);

        // The roots the game marks, so a camera's cull mask can keep or leave out what stands
        // under them — `rayMaskOf` is the other half.
        mExtractor.setClassMask(Rtx::InstanceClass::Actor, SceneUtil::Mask_Actor | SceneUtil::Mask_Player);
        mExtractor.setClassMask(Rtx::InstanceClass::Effect, SceneUtil::Mask_Effect);
        mExtractor.setClassMask(Rtx::InstanceClass::FirstPerson, SceneUtil::Mask_FirstPerson);
    }

    void WorldMirror::attach(Resource::ResourceSystem& resources)
    {
        mResources = &resources;
        mContent = std::make_unique<Rtx::SceneContent>(*resources.getSceneManager());
    }

    void WorldMirror::detach()
    {
        // **The ring first, because its thread reads the storages the world owns.** A world with
        // nothing in it is what stops the thread and drops what it held.
        mRing.follow(Rtx::WorldAround{});
        mDistantLights.follow(Rtx::WorldAround{});

        mContent.reset();
        mResources = nullptr;
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
        // **The world's clock and not this renderer's.** Everything the graph animates under its own
        // controller reads it off the walk's frame stamp, and the sea off the frame's constants; a
        // clock of our own would run both while the game was paused and neither in step with the
        // time of day.
        mExtractor.setSimulationTime(frame.mWhen.getSimulationTime());

        // **The emitters on their own clock, by the gap and not to the time.** They are the one
        // thing here that integrates the difference between two frames rather than reading the
        // hour, so what a pause or a loading screen leaves in that difference is a jump they would
        // take literally. The extractor clamps it; this only has to hand over the gap.
        mExtractor.advanceEmitters(frame.mWhen.getSimulationTime() - mLastSimulationTime);
        mLastSimulationTime = frame.mWhen.getSimulationTime();

        // **Every frame, and the placements are the one thing it does not throw away.** What goes
        // is the lists a walk refills wholesale — lights, deformed meshes, sprites, emitters. The
        // meshes, materials and texture paths stay because the acceleration structures and the
        // texture array were built from them, and the placements stay because they are addressed by
        // slot: a re-walk over an unchanged graph finds every one of them where it left it.
        mScene.clearPlacement();

        // **The moons' portraits, once, into the table the trace reads.** Held rather than named by
        // a material: a moon is drawn by a ray that reached nothing, so nothing else can speak for
        // the slot and the sweep would take it on the first frame a cell died. The scene outlives
        // every cell here, so this is asked once and never again.
        if (mMoonFaces.mMasser == Rtx::sNoIndex)
        {
            mMoonFaces = Rtx::addMoonFaces(mScene);
            mSkyContent = Rtx::addSkyContent(mScene, *mResources->getSceneManager(),
                Rtx::SkyMeshes{ .mClouds = Settings::models().mSkyclouds,
                    .mStars = Settings::models().mSkynight02,
                    .mStarsFallback = Settings::models().mSkynight01 });
        }

        // **What the weather drops, walked as a second root**, which `Rtx::mirrorPrecipitation`
        // is the whole of: the sky's own mask keeps the world walk out of that subtree entirely,
        // and it is right that it does — a cloud deck is a texture on a ray that reached nothing,
        // and rain is geometry standing in front of one.
        //
        // The same systems the rasterizer draws, not a second set of them: the sky manager builds
        // them and neither renderer owns them.
        //
        // **The eye, which stands the drops and decides the rings.** Where it stands is what the
        // reach is measured from.
        const osg::Vec3f eye = frame.mCamera.getInverseViewMatrix().getTrans();
        Rtx::mirrorPrecipitation(mExtractor, frame.mWorld.mRain, eye, frame.mWorld.mUnderwater, frameNumber);
        Rtx::mirrorPrecipitation(mExtractor, frame.mWorld.mWeatherEffect, eye, frame.mWorld.mUnderwater, frameNumber);

        // **The same eye, the same reach and the world's own grid, said once.** What the game has
        // stood for itself is what neither residency may stand again, and `Terrain::World` is where
        // both renderers read that from. Told to each of them as one value: they read all of
        // these, and a fact stated twice is one that can be stated differently.
        //
        // **The world inside it is the land the heights are read from, the loader the models come
        // out of, and the template walk's own mask** — the three only the ring reads, stated here
        // beside the two both residencies do rather than told to the ring by a call of its own.
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
            .mOutdoors = !frame.mWorld.isInteriorCell(),
        };

        mRing.setFrame(frameNumber);

        // Told once a frame, because what the graph does not hold is the frame's to say. Every
        // world walk asks it from here, and the precipitation walk above cannot: it is a subtree.
        std::array<Rtx::Residency*, 2> hidden{ &mDistantLights, &mRing };
        for (Rtx::Residency* resident : hidden)
            resident->follow(around);

        mExtractor.follow(hidden);

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
        // **After the frame and not before the walk.** Where everything stood this frame is what
        // the next one measures its motion against, and saying so any earlier would have this frame
        // comparing itself with itself.
        //
        // On the frames the trace refused as well: the walk still ran, so its epoch is still the
        // one the next walk has to be measured against.
        mExtractor.advance();

        // **What the walk did not find has gone, and this is where the scene is told.** The graph
        // above is the whole world every frame, which is what makes mark and sweep sound here — and
        // it is also the only thing that lets go: the identity maps hold their keys alive, so
        // geometry the engine has dropped outlives it until a sweep takes the entry naming it.
        //
        // Last, because it bumps the epoch the next walk is measured against: everything that
        // survived is still carrying the old stamp until it does.
        if (const Rtx::Retirement went = mExtractor.retire(); !went.empty())
            Log(Debug::Info) << "Ray tracing dropped " << went.mMeshes << " meshes and " << went.mMaterials
                             << " materials the world no longer has";
    }

    Rtx::WorldReading WorldMirror::readWorld(const WorldState& world, const float seconds) const
    {
        // **Where the sun *is*, and the light comes back along it.** The world also reports
        // `mSunVector`, which is where the rasterizer's light travels and is not the negation of
        // this — `MWWorld::WeatherManager::update` says why, and why nothing that traces can hold both.
        osg::Vec3f discAt(world.mSunPosition.x(), world.mSunPosition.y(), world.mSunPosition.z());
        if (discAt.length2() > 0.0f)
            discAt.normalize();

        // **A room's light is built once, out of the record the cell wrote** — `Rtx::makeRoomLight`,
        // which is what `openmw-rtxtool` reads out of the content files — and not out of the
        // rasterizer's reading of it. `mAmbientColour` carries the lift `configureAmbient` gives an
        // interior for its own falloff curve and `mSunPosition` where it points a directional
        // light, and neither is a fact about the room. What the game adds for Night-Eye comes over
        // as itself.
        const std::optional<Rtx::Daylight> room = world.mRoom.has_value()
            ? std::optional(Rtx::makeRoomLight(
                *world.mRoom, osg::Vec3f(world.mNightEye.x(), world.mNightEye.y(), world.mNightEye.z())))
            : std::nullopt;

        // The horizon is the fog and the zenith is the sky's own, which is the pair Morrowind
        // records: one colour for the air, and one for the dome it fades into overhead.
        //
        // **Decoded here, because the world does not know what a transport is.** Every colour on
        // the frame is a content file's three bytes over 255 and no transfer function; the
        // rasterizer samples them as they are and this light transport is linear, so the conversion
        // belongs to whichever renderer needs it.
        const osg::Vec3f haze = room.has_value() ? room->mSkyHorizon : Rtx::decodeColour(world.mAir.mColour);

        // **The sky's own colour, and an interior has none.** The weather system stops writing it
        // the moment the player steps inside, so what the sky is still holding belongs to wherever
        // they were last outdoors — and the air's own colour stands in, which is what a room's sky
        // is anyway. A quasi-exterior is on the outdoor side of that: it has weather.
        const osg::Vec3f zenith = room.has_value() ? room->mSkyZenith : Rtx::decodeColour(world.mSkyColour);

        // **The sun is not assembled here.** Everything the world says about it goes to the one
        // builder that decides what a sun may be — which is what makes a sun that lights an empty
        // night impossible to write. A room has none, and `Rtx::makeRoomLight` is where that is
        // said.
        //
        // **Taken whole from whichever built it**, the exposure bias included: a light is one thing
        // and this is the choice between two of them, not a place to reassemble either.
        const Sky::TimeOfDaySettings& times = Sky::TimeOfDaySettings::shared();
        const Rtx::Skylight light = room.has_value()
            ? room->mLight
            : Rtx::makeSkylight(Rtx::SkyReading{
                .mSunPosition = discAt,

                // **Off the hour rather than off the disc's alpha**, which the rasterizer leaves at
                // one all night with the disc hidden. The deck keeps the sun after the ground has
                // lost it, and the same hour says how much of it is left — `Rtx::sunShareAloft`.
                .mSunShare = Rtx::sunShareAt(world.mGameHour, times),
                .mSunShareAloft = Rtx::sunShareAloft(world.mGameHour, times),
                .mSunColour = Rtx::decodeColour(world.mSunColour),
                .mAmbient = Rtx::decodeColour(world.mAmbientColour),
                .mDiscColour = Rtx::decodeColour(world.mSunDiscColour),
                .mGlare = world.mSunGlare,
            });

        // **The recorded depth, and not the ramp `MWRender::FogManager` made of it.** That ramp
        // exists to hide a far clip plane and this renderer has no far clip to hide, so per the
        // fork's own rule it does not come across. What is read instead is the number the content
        // wrote, handed to the one builder that knows what a cell's air may be — which is what
        // `openmw-rtxtool` calls out of the same records, so a screenshot and a played frame stand
        // in one air.
        //
        // **A quasi-exterior stands in the weather's air, because that is the air the game gives
        // it.** The weather system is run for one, so what arrives here is already a weather's
        // blended `Land_Fog_Depth` and fog colour rather than anything of the cell's — the
        // Construction Set greys the whole `AMBI` record out for a cell that behaves like an
        // exterior, so there is nothing of a room's to read. Handed to `roomFog` all the same, the
        // even unbanked medium it builds has no layer for a ray to climb out of: it closed over the
        // sky, and the sun with it.
        //
        // **The two open-air builders read the same four numbers**, and differ only in the ring
        // they close over, so the cell picks between the builders rather than between two calls.
        const auto openAir = world.mLocation == Location::Exterior ? &Rtx::exteriorFog : &Rtx::quasiExteriorFog;
        const Rtx::Fog air
            = room.has_value() ? room->mFog : openAir(haze, world.mFogDepth, world.mBaseWindSpeed, mReach);

        // **Before the frame rather than into it, because the deck is lit by them.** A cloud layer
        // takes the moons' light like anything else under a night sky, and `Rtx::deckLight` is
        // handed the pair.
        std::array<Rtx::MoonPlacement, 2> moons{};
        for (std::size_t moon = 0; moon < moons.size(); ++moon)
        {
            const Sky::MoonState& state = world.mMoons[moon];

            // **The glare is applied here and not by the weather system**, which is where the
            // rasterizer applies it too: `SkyManager::setWeather` calls `Moon::adjustTransparency`
            // with it after the state has been handed over. A thunderstorm hides its moons the same
            // way it hides its stars.
            moons[moon] = Rtx::placeMoon(static_cast<Rtx::Moon>(moon), state.mRotationFromHorizon,
                state.mRotationFromNorth, state.mPhase, state.mDaylightFade * world.mSunGlare);
            moons[moon].mFace = mMoonFaces.of(static_cast<Rtx::Moon>(moon));
        }

        const auto weatherId = static_cast<std::uint32_t>(world.mWeatherId);

        return Rtx::WorldReading{
            .mDaylight = Rtx::Daylight{
                .mLight = light,
                .mSkyHorizon = haze,
                .mSkyZenith = zenith,
                .mStarFade = world.mNightFade,
                .mFog = air,
            },
            .mOutdoors = world.isOutdoors(),
            .mGlare = world.mSunGlare,
            .mStarRoll = world.mStarRoll,
            .mSky = mSkyContent,
            .mMoons = moons,
            .mClouds = Rtx::CloudCrossing{
                .mWeather = weatherId,
                // The current weather twice where nothing is arriving, since the deck crosses
                // unconditionally: naming it on both sides at a blend of nothing is what lets it.
                .mNext = world.mNextWeatherId.has_value() ? static_cast<std::uint32_t>(*world.mNextWeatherId)
                                                          : weatherId,
                .mBlend = world.mCloudBlend,
                .mDirection = world.mCloudDirection,
                .mNextDirection = world.mNextCloudDirection,
                .mScroll = world.mCloudScroll,
            },

            // Negative infinity and not zero: zero is sea level, and a cell with no water has to
            // answer "how deep is this point" with never.
            .mWaterLevel = world.mWaterEnabled ? world.mWaterHeight : -std::numeric_limits<float>::infinity(),

            // **What the sea is animated by, and leaving it at zero is a frozen ocean.** Real
            // elapsed seconds rather than the frame count: a sea that ran at the frame rate would
            // slow down whenever the frame did.
            .mSeconds = seconds,
            .mRainOnWater = world.mRainOnWater,
        };
    }
}
