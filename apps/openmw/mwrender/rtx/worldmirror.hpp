#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include <osg/PositionAttitudeTransform>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/esm3/refnum.hpp>
#include <components/rtx/cellring.hpp>
#include <components/rtx/compositequeue.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/framespend.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/residency.hpp>
#include <components/rtx/ripple.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/skybuilder.hpp>
#include <components/rtx/walk.hpp>

namespace Resource
{
    class ImageManager;
    class ResourceSystem;
}

namespace MWWorld
{
    class CellStore;
}

namespace Rtx
{
    class Renderer;
}

namespace MWRender
{
    struct SceneFrame;
    struct WorldState;

    /// The engine's scene graph mirrored into what a ray can meet.
    ///
    /// **Everything between "the game has a frame" and "trace it".** The walk, what it walks past,
    /// what this renderer stands for the ground and the distance, the lights the game has already
    /// placed, and the hand-over that decides whether the device is placed, extended or rebuilt.
    /// Nothing here touches a window, an event, the interface or a benchmark.
    ///
    /// **The maps live across frames**, which is what makes the mirror incremental: the same crate
    /// met again resolves to the mesh already uploaded rather than to a copy of it, and a cell that
    /// left gives its slots back on the frame the sweep misses it.
    class WorldMirror
    {
    public:
        /// Reads three settings once: the two knobs the paging read for the distance's statics,
        /// which this renderer stands itself, and how far out the world is built.
        ///
        /// **Once, because a frame reads what it was handed.** The reach is one number for the
        /// ground, the air, the distant lights and the checks; a host that asked the registry per
        /// frame could answer it differently in each. The statics need a restart; the reach follows
        /// the menu through `setReach`.
        WorldMirror();

        /// The resource system the sky's own meshes are loaded through, and the cell ring's models
        /// and images with them. Told once, where the world is attached.
        void attach(Resource::ResourceSystem& resources);

        /// The world is going: every thread that reads it stops, and what was read of it goes.
        void detach();

        /// Walks this frame's world into the scene, and says what the walk found.
        ///
        /// The precipitation and the sea go in as roots of their own: the precipitation because it
        /// hangs under a camera-relative transform the world walk is masked out of, the sea because
        /// this renderer stands it — the plane the rasterizer's `Water` stood was the sea a ray met,
        /// and that object is the rasterizer's now.
        Rtx::ExtractionStats mirror(const SceneFrame& frame, std::size_t frameNumber);

        /// What disturbed the water this frame, into the scene the walk just cleared, so the
        /// trace presses it and the digest sees it. After `mirror`, which clears the frame's lists.
        void addRipples(std::span<const Rtx::RippleImpulse> impulses);

        /// A cell the scene added, which is what the sea is centred on: upstream's
        /// `Water::changeCell`, verbatim in effect — the middle of the cell outdoors, the origin
        /// indoors, the last one added winning. The plane is a hundred and fifty cells wide, so
        /// where its middle is does not show; kept the rasterizer's so the two pictures agree.
        void standSea(const MWWorld::CellStore& cell);

        /// Hands the scene to `renderer`, building only what has to be built.
        Rtx::SceneUpload hand(Rtx::Renderer& renderer, Resource::ImageManager& images, Rtx::FrameSpend& spend);

        /// Whether each hand-over waits for the composites it collects, and each walk for the one
        /// cell it adopts. `Rtx::CompositeQueue::setSettled` and `Rtx::CellRing::setSettled` say why
        /// a run would, and what waiting costs it.
        void setSettled(bool settled)
        {
            mComposites.setSettled(settled);
            mRing.setSettled(settled);
        }

        /// What the game says of one reference, which the content files cannot: a script has
        /// disabled it, or enabled it again.
        void setReferenceEnabled(ESM::RefNum refnum, bool enabled) { mRing.setReferenceEnabled(refnum, enabled); }

        /// How much world this renderer builds, in units: the ground, the air and the distant
        /// lights are all measured over it — `Rtx::distantLandReach`, as the settings stood when
        /// the mirror was made or when the menu last moved them.
        float getReach() const { return mReach; }

        /// The menu moved the reach, or the view distance it falls back to. Told rather than read
        /// per frame, so the ring, the air and the map follow one number a frame was handed.
        void setReach(float reach) { mReach = reach; }

        /// Where the last walk stood the rings: the camera's eye, which is not the player's feet.
        const osg::Vec3f& getEye() const { return mEye; }

        /// Whether the world walk includes the player's own model. True for a game somebody is
        /// playing.
        ///
        /// **A camera that is not the player's eye stands inside the player.** `MWRender::Camera` in
        /// `Mode::Static` takes the `VM_Normal` branch of `processViewChange`, so the game dresses
        /// the whole third-person body — and a session flies the player to its route's point so that
        /// cells load around it, then stands the camera on the same coordinates. What that traced
        /// was a boot and a trouser leg thirteen units from the eye, filling a third of the frame.
        void setShowsPlayer(bool shows);

        /// Catches the walk up and drops what it did not find.
        ///
        /// **After the trace and not before the walk.** Where everything stood this frame is what
        /// the next one measures its motion against, and the sweep bumps the epoch that measurement
        /// is made against.
        void settle();

        const Rtx::SceneDesc& getScene() const { return mScene; }
        Rtx::SceneDesc& getScene() { return mScene; }

        /// Where every walk that can reach one graph takes its traversal numbers from.
        Rtx::Traversals& getTraversals() { return mTraversals; }

        // Read by the tests and by nothing else.
        osg::Node::NodeMask getTraversalMask() const { return mExtractor.getTraversalMask(); }

        /// Turns what the game says about this frame's world into what the renderer builds a sky,
        /// an air and a sea out of: the colours decoded, whether the cell has a sky decided, and
        /// every reading handed to the one builder that decides what a sun, a room light, an air
        /// and a moon may be — which is what keeps the game and the harness under the same sky.
        /// Nothing here touches a device, and nothing here is a decision this host makes on its own.
        ///
        /// @param seconds the world's clock, which the sea is animated by.
        Rtx::WorldReading readWorld(const WorldState& world, float seconds) const;

    private:
        /// Shared by everything that can reach one graph — the world's walk and every traced view.
        Rtx::Traversals mTraversals;

        Rtx::SceneDesc mScene;
        Rtx::SceneExtractor mExtractor;

        bool mShowsPlayer = true;

        /// The moons' portraits and the sky's own meshes, added once and never given back.
        Rtx::MoonFaces mMoonFaces;
        Rtx::SkyContent mSkyContent;

        /// What a script paints Secunda, `Moons_Script_Color` decoded, read once as the
        /// rasterizer's `SkyManager` reads it. `SkySettled::mMoonRed` says when.
        osg::Vec3f mMoonPaint;

        /// The sun glare fader's three constants, read once as `SunGlareCallback` reads them:
        /// `Weather_Sun_Glare_Fader_Color` doubled and clamped, `_Max`, and `_Angle_Max` in
        /// radians. `Rtx::Shaders::glare.h` says what each is.
        osg::Vec3f mGlareColour;
        float mGlareMax;
        float mGlareAngleMax;

        /// The sea: upstream's water geometry under `Mask_Water`, which is how the extractor
        /// knows a sea from a floor, stood at the frame's water height and hidden where the frame
        /// says there is none. Made once; a frame moves it.
        osg::ref_ptr<osg::PositionAttitudeTransform> mSea;
        osg::Vec2f mSeaCentre;

        /// The cells themselves: their ground off the land records, their statics as instances
        /// of their templates, and their lamps. After the scene, which it adopts into.
        Rtx::CellRing mRing{ mScene };

        /// Where the ring's models and images come from: the game's own. Made where the world is
        /// attached, because that is when there is a scene manager.
        std::unique_ptr<Rtx::ContentSource> mContent;

        Rtx::SceneUploader mUploader;

        /// The distant cells waiting for their ground to be flattened, and the thread flattening
        /// them.
        ///
        /// **Here because only a world has ground.** A bake outlives the frame that asked for it,
        /// so it belongs to what outlives frames rather than to the once-a-frame call — and every
        /// picture inside the interface goes through that same call with no ground to flatten.
        Rtx::CompositeQueue mComposites;

        /// Where the sky's meshes are loaded from. Borrowed: the world outlives this.
        Resource::ResourceSystem* mResources = nullptr;

        /// Where the world's clock stood on the last frame, so the emitters are given the gap.
        double mLastSimulationTime = 0.0;

        float mReach;
        osg::Vec3f mEye;
    };
}
