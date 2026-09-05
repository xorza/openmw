#include "worldmirror.hpp"

#include <array>

#include <components/debug/debuglog.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/distantland.hpp>
#include <components/rtx/frametimes.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/renderer.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/settings/values.hpp>
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
    }

    float landReach()
    {
        return Rtx::distantLandReach(Settings::rtx().mDistantLandCells, Settings::camera().mViewingDistance);
    }

    WorldMirror::WorldMirror()
        : mExtractor(mScene, &mTraversals)
    {
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
        mExtractor.setTraversalMask(sWorldTraversal & ~NifOsg::Loader::getHiddenNodeMask());

        // What is left of the two is the world's own water, and it is the sea.
        mExtractor.setWaterMask(SceneUtil::Mask_Water);
        mExtractor.setFirstPersonMask(SceneUtil::Mask_FirstPerson);
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

        // **Where the benchmark's `walk ms` starts**, because that row means the whole mirror and
        // the precipitation subtree below is part of it. The harness times the same stretch, which
        // is what lets the two rows be read against each other.

        // **What the weather drops, walked as a second root**, which `Rtx::mirrorPrecipitation`
        // is the whole of: the sky's own mask keeps the world walk out of that subtree entirely,
        // and it is right that it does — a cloud deck is a texture on a ray that reached nothing,
        // and rain is geometry standing in front of one.
        //
        // The same systems the rasterizer draws, not a second set of them. `Weather::Precipitation`
        // owns them and neither renderer does.
        Rtx::mirrorPrecipitation(mExtractor, frame.mWorld.mPrecipitation, frameNumber);

        // **The eye, which is what a cull would have used.** The detail a chunk is built at has to
        // be the detail the primary rays hit, and asking from anywhere else would put the ground a
        // reflection sees at a different level from the ground beside it.
        const osg::Vec3f eye = frame.mCamera.getInverseViewMatrix().getTrans();

        mResident.follow(&frame.mTerrain);
        mResident.setViewPoint(eye);

        // **The same eye and the world's own grid.** What the game has stood for itself is what
        // these must not stand again, and `Terrain::World` is where both renderers read that from.
        mDistantLights.follow(&frame.mObjectStorage, frame.mTerrain.getWorldspace());
        mDistantLights.setViewPoint(eye);
        mDistantLights.setReach(landReach());
        mDistantLights.setActiveGrid(frame.mTerrain.getActiveGrid());
        mDistantLights.setOutdoors(!frame.mWorld.isInteriorCell());

        // Told once a frame, because what a paged world hides is the frame's to say. Every world
        // walk asks it from here, and the precipitation walk above cannot: it is a subtree.
        std::array<Rtx::Residency*, 2> hidden{ &mResident, &mDistantLights };
        mExtractor.follow(hidden);

        // One walk over the whole graph, where every path is already distinct.
        return mExtractor.extractWorld(frame.mScene, osg::Matrixf::identity(), 0, frameNumber);
    }

    Rtx::SceneUpload WorldMirror::hand(Rtx::Renderer& renderer, Resource::ImageManager& images)
    {
        return mUploader.hand(renderer, Rtx::sWorld, mScene, images, Rtx::SeaState{});
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
}
