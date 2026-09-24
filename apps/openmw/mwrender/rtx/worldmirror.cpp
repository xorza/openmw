#include "worldmirror.hpp"

#include <cassert>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <string>

#include <osg/Geometry>
#include <osg/Image>
#include <osg/Matrixd>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/PositionAttitudeTransform>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/misc/constants.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/camera.hpp>
#include <components/rtx/cellgrid.hpp>
#include <components/rtx/cellworld.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/result.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/slot.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/sceneutil/waterutil.hpp>
#include <components/terrain/world.hpp>
#include <components/vfs/pathutil.hpp>

#include "../../mwworld/cellstore.hpp"
#include "../precipitation.hpp"
#include "../sceneframe.hpp"
#include "../vismask.hpp"
#include "classmasks.hpp"

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

            Rtx::Result<osg::ref_ptr<const osg::Image>, std::string> getImage(VFS::Path::NormalizedView path) override
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
        /// The ground is the ring's: what `TracedTerrain` stands under `Mask_Terrain` is the
        /// intersector's, and walked it would place every loaded cell's ground a second time.
        ///
        /// **`Mask_UpdateVisitor` is what the content hides with, and it is named here rather than
        /// asked of `NifOsg::Loader`.** The loader is told that bit by `RenderingManager`, and this
        /// renderer is built before there is a rendering manager to tell it, so a mask that reads
        /// the loader's answer subtracts nought: every node a `NifOsg::VisController` hides is
        /// traced anyway, and the Heart of Lorkhan stands wearing the whole of its destruction at
        /// the frame that sequence opens on. The bit is the engine's own decision and a constant,
        /// so it is stated as one and no order can reach it — which is what
        /// `MWRender::ObjectPaging` does with the same bit for the same reason.
        /// `WorldMirror::mirror` checks the loader against it.
        constexpr osg::Node::NodeMask sWorldTraversal = ~static_cast<osg::Node::NodeMask>(
            Mask_Sky | Mask_Sun | Mask_SimpleWater | Mask_Terrain | Mask_UpdateVisitor);

        /// What each walk this renderer makes over the one extractor is anchored at. Four roots the
        /// walk cannot tell apart by structure, so each is named; the world's is nought, which is
        /// what `SceneExtractor::extract` calls a caller that walks one whole graph.
        enum Anchor : std::size_t
        {
            World = 0,
            Sea = 1,
            Rain = 2,
            Effect = 3,
        };

        /// What the world walk may see: `sWorldTraversal` without the player bit, which is stamped
        /// on nothing a content file holds. `WorldMirror::setShowsPlayer` says why the player is a
        /// question.
        constexpr osg::Node::NodeMask worldTraversal(const bool showsPlayer)
        {
            const osg::Node::NodeMask player = showsPlayer ? 0 : static_cast<osg::Node::NodeMask>(Mask_Player);

            return sWorldTraversal & ~player;
        }
    }

    WorldMirror::WorldMirror(const Rtx::MirrorKnobs& knobs)
        : mExtractor(mScene, &mTraversals)
        , mReach(knobs.mReach)
        , mSpecularLayout(knobs.mSpecularLayout)
    {
        mRing.setStaticsEnabled(knobs.mDistantStatics);
        mRing.setMinSize(knobs.mMinSize);
        // The sky is not mirrored: the engine rebuilds it every frame, state sets and all, so
        // walking it churns the identity maps and makes every frame a full rebuild, and a ray that
        // reaches the sky gets this renderer's own. The simple water is the local map's copy of the
        // sea, which a mirror walking both would place twice. What the content hides is the one bit
        // `sWorldTraversal` names, rather than none, so the update traversal still reaches a hidden
        // bone.
        mExtractor.setTraversalMask(worldTraversal(mShowsPlayer));

        // Where the engine stamps its identities: the cell roots under the scene root and the
        // reference roots under those, and the player beside the cells (`MWRender::Objects`).
        mExtractor.setStampDepth(2);

        // What is left of the two is the sea, which this renderer stands: upstream's plane, as
        // `MWRender::Water` makes it, on a transform a frame moves.
        mExtractor.setWaterMask(Mask_Water);
        mExtractor.setSpecularLayout(mSpecularLayout);

        osg::ref_ptr<osg::Geometry> sea = SceneUtil::createWaterGeometry(Constants::CellSizeInUnits * 150, 40, 900);
        sea->setNodeMask(Mask_Water);
        sea->setName("Sea Geometry");
        mSea = new osg::PositionAttitudeTransform;
        mSea->setName("Sea Root");
        mSea->addChild(sea);

        // The roots the game marks, so a camera's cull mask can keep or leave out what stands
        // under them — `rayMaskOf` reads the same table the other way.
        for (const ClassMask& held : sClassMasks)
            if (held.mClass != Rtx::InstanceClass::Static)
                mExtractor.setClassMask(held.mClass, held.mNodes);
    }

    void WorldMirror::attach(Resource::ResourceSystem& resources)
    {
        mImages = resources.getImageManager();
        mContent = std::make_unique<SceneContent>(*resources.getSceneManager());
    }

    WorldMirror::~WorldMirror()
    {
        assert((std::uncaught_exceptions() > 0 || mScene.isEmpty()) && "a world detached and still standing rows");
    }

    void WorldMirror::detach()
    {
        // **The ring's thread reads the storages the world owns.** A world with nothing in it is
        // what stops the thread and drops what it held.
        mRing.follow(Rtx::WorldAround{});

        // What the ring held on the extractor's rows goes back, and the sweep frees them with
        // everything the walks stood: nothing of this world stays in the scene. Not on the way
        // out of an exception a frame threw, where the scene is whatever the throw left and the
        // assert in the retire would stand between the throw and its message.
        if (std::uncaught_exceptions() == 0)
            mExtractor.detach(mRing);

        mContent.reset();
        mImages = nullptr;
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

    Rtx::ExtractionStats WorldMirror::mirror(
        const SceneFrame& frame, const osg::Matrixd& view, const std::size_t frameNumber)
    {
        // **Whatever the loader hides with has to be inside what this mask excludes.**
        // `sWorldTraversal` names that bit rather than asking, because the loader is told it by
        // `RenderingManager` and this renderer is built first — so the two are checked against each
        // other here instead, where a walk is about to use the mask. A loader nobody has told yet
        // hides with nothing and passes: what this catches is the engine moving the bit.
        assert((NifOsg::Loader::getHiddenNodeMask() & ~static_cast<unsigned int>(Mask_UpdateVisitor)) == 0
            && "the loader hides with a bit the walk's mask does not exclude");

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

        // What the weather drops, walked as a second root, because the sky's mask keeps the world
        // walk out of that subtree: the same systems the rasterizer draws, stood at the eye.
        const osg::Matrixd inverseView = osg::Matrixd::inverse(view);
        const osg::Vec3f eye = inverseView.getTrans();
        mEye = eye;

        // And the eye every billboard in the world turns to, which the rasterizer's cull hands its
        // `AutoTransform`s and this walk has to be told.
        mExtractor.setEye(Rtx::viewBasisOf(inverseView));
        Rtx::mirrorPrecipitation(
            mExtractor, frame.mPrecipitation.getRainNode(), eye, frame.mWorld.mUnderwater, Anchor::Rain, frameNumber);
        Rtx::mirrorPrecipitation(mExtractor, frame.mPrecipitation.getParticleNode(), eye, frame.mWorld.mUnderwater,
            Anchor::Effect, frameNumber);

        // The sea, where the frame says there is one: hidden by its mask otherwise, as the
        // rasterizer's `updateVisible` hid the same plane, so the walk leaves no placement of it.
        mSea->setPosition(osg::Vec3f(mSeaCentre.x(), mSeaCentre.y(), frame.mWorld.mWater.mHeight));
        mSea->setNodeMask(frame.mWorld.mWater.isShown() ? ~0u : 0u);
        mExtractor.extract(*mSea, osg::Matrixf::identity(), Anchor::Sea, frameNumber);

        // The eye, the reach, the world's own grid and the hour, said once to the ring: what the
        // game has stood for itself is what the ring may not stand again, and its lamps burn at
        // the world's clock as the graph's do.
        const Rtx::WorldAround around{
            .mWorld = {
                .mStorage = &frame.mObjectStorage,
                .mGround = frame.mTerrain.getStorage(),
                .mContent = mContent.get(),
                .mWorldspace = frame.mTerrain.getWorldspace(),
                // The world's mask and never the walk's own: a mask that moves makes
                // `Rtx::CellRing::forget` read the ring from nothing, and the player bit moves
                // while the camera settles.
                .mMask = sWorldTraversal,
            },
            .mEye = eye,
            .mReach = mReach,
            .mActiveGrid = frame.mTerrain.getActiveGrid(),
            .mExterior = !frame.mWorld.isInteriorCell(),
            .mSimulationTime = frame.mWhen.getSimulationTime(),
        };

        // Told once a frame, because what the graph does not hold is the frame's to say. The
        // world walk asks it, and the precipitation walk above cannot: it is a subtree. Which
        // frame the ring is standing for is `extractWorld`'s to say, because that call has the
        // ring and the number both.
        mRing.follow(around);

        // One walk over the whole graph, where every path is already distinct.
        const Rtx::ExtractionStats found
            = mExtractor.extractWorld(frame.mScene, osg::Matrixf::identity(), Anchor::World, frameNumber, mRing);

        // What the walks did not find has gone. The graph is the whole world every frame, which is
        // what makes mark and sweep sound; the identity maps hold their keys alive until it runs.
        // After every walk of the frame and never before one, because the sweep bumps the epoch
        // the next walk is measured against.
        mExtractor.retire();

        return found;
    }

    void WorldMirror::addRipples(std::span<const Rtx::RippleImpulse> impulses)
    {
        for (const Rtx::RippleImpulse& impulse : impulses)
            mScene.addRipple(impulse);
    }

    Rtx::SceneUpload WorldMirror::hand(Rtx::Renderer& renderer, Rtx::FrameSpend& spend)
    {
        assert(mImages != nullptr && "a hand-over before the world was attached");

        return mUploader.hand(renderer,
            Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(),
                .mScene = mScene,
                .mImages = *mImages,
                .mComposites = &mComposites,
                .mSpend = &spend });
    }

}
