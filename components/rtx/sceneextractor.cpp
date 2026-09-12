#include "sceneextractor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/Sequence>
#include <osgParticle/Particle>
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>

#include <components/nifosg/nifloader.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/skeleton.hpp>
// `terraindrawable.hpp` holds `osg::ref_ptr`s to composite-map types it only forward-declares, so it
// does not compile on its own. This is what completes them.
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

#include "lightbuilder.hpp"
#include "nodekind.hpp"
#include "worlddescent.hpp"

namespace Rtx
{
    namespace
    {
        /// Clears the one gate a renderer with no draw can only ever answer wrongly:
        /// `osgParticle` stops a system whose draw has not touched it for two frames, and
        /// `ParticleSystem::_last_frame` moves in `drawImplementation` and nowhere else.
        void keepRunning(osgParticle::ParticleSystem& system)
        {
            if (system.getFreezeOnCull())
                system.setFreezeOnCull(false);
        }

        constexpr std::size_t sPlacementBudget = 65536;
        constexpr std::size_t sMeshBudget = 16384;
        constexpr std::size_t sMaterialBudget = 16384;
        constexpr std::size_t sTextureBudget = 8192;
        constexpr std::size_t sDeformerBudget = 2048;
        constexpr std::size_t sAnimatedBudget = 4096;
        constexpr std::size_t sEmitterBudget = 2048;

        /// What identifies one placement from one frame to the next: the anchor a walk starts from
        /// and the node path under it, together, because a hundred crates share one geometry and,
        /// walked from a shared template node, one path as well. Hashed rather than kept, because a
        /// path is a vector of pointers per placement; folded on the way down, so the prefix every
        /// sibling shares is worked out once.
        std::size_t identityWith(std::size_t key, const std::size_t part)
        {
            return (key ^ part) * 0x100000001b3ull;
        }

        std::size_t identitySeed(std::size_t anchor)
        {
            return identityWith(0xcbf29ce484222325ull, anchor);
        }

        std::size_t identityWith(std::size_t key, const osg::Node* node)
        {
            return identityWith(key, std::hash<const osg::Node*>{}(node));
        }

    }

    /// Runs an `osg::Sequence`'s clock, and reaches nothing. A visitor of its own, because
    /// `Sequence::traverse` moves its clock only for an update traversal in
    /// `TRAVERSE_ACTIVE_CHILDREN`, and neither claim is true of the mirror.
    struct SequenceClock : osg::NodeVisitor
    {
        SequenceClock()
            : osg::NodeVisitor(UPDATE_VISITOR, TRAVERSE_ACTIVE_CHILDREN)
        {
        }

        /// The frame the sequence settles on is walked by the mirror afterwards, not by this —
        /// which is what keeps a flipbook's subtree from being reached twice a frame.
        void apply(osg::Node&) override {}
    };

    /// Walks the graph and hands every geometry it meets to the extractor.
    class MirrorTraversal : public osg::NodeVisitor
    {
    public:
        explicit MirrorTraversal(SceneExtractor& extractor);

        /// Points the walk at a root, at where it stands, and at the frame it is mirroring.
        void begin(const osg::Matrixf& root, std::size_t frame, unsigned int traversal, std::size_t identity);

        osg::FrameStamp& getStamp() { return *mStamp; }

        /// Moves the emitter clock on by one frame. See `mEmitterStamp`.
        void advanceEmitters(double elapsed);

        /// Runs the emitters under `node` and looks through everything else, for a caller that wants
        /// them moved without a frame being mirrored.
        void stepOnly(osg::Node& node);

        void apply(osg::Node& node) override;
        void apply(osg::Transform& node) override;
        void apply(osg::Drawable& drawable) override;

    private:
        /// Walks `node` and everything under it, under the identity the caller worked out for it.
        void enter(osg::Node& node, std::size_t identity);

        /// The same, with `node`'s own transform composed into where the walk stands.
        void enterTransform(osg::Transform& node, std::size_t identity);

        /// Descends into the children of `node` that are in the world. See below.
        void descend(osg::Node& node, NodeKind kind);

        /// Puts `stateSet` at the near end of the chain, with the fade resolved through it.
        void pushShading(const osg::StateSet& stateSet, bool animated);

        /// Runs one node of an `osgParticle` simulation, if that is what this node is. See below.
        ///
        /// @param from the node's library, which `apply` has already asked for.
        bool stepParticles(osg::Node& node, NodeKind kind);

        /// Where the node being visited stands in the world, narrowed to single precision here and
        /// not before, so a placement lands on the bits `computeLocalToWorld` would have landed it.
        osg::Matrixf placed() const { return osg::Matrixf(mHere) * mRoot; }

        SceneExtractor& mExtractor;

        /// What kind each class of *node* this walk meets is. A member because the answers are a
        /// fact about the classes in the world rather than about one frame; a drawable is dispatched
        /// to its own `apply` and never reaches the one that asks about a node, so `SceneExtractor`
        /// holds a classifier of its own for those.
        NodeKinds mKinds;

        /// The clock every controller under this walk reads. Its simulation time is the world's;
        /// its frame number is the walk's own, for the reason `begin` gives.
        osg::ref_ptr<osg::FrameStamp> mStamp = new osg::FrameStamp;

        /// A member for the reason the walk is: made once, and a frame allocates none of it.
        SequenceClock mSequenceClock;

        /// The emitters' own clock, and it is not the world's: `osgParticle` integrates the
        /// difference between one frame stamp and the last, and the world's clock jumps across a
        /// loading screen. Its frame number is the sequence `ParticleProcessor` keeps its
        /// once-per-frame guard against, which is why nothing else in this renderer may drive a
        /// particle system.
        osg::ref_ptr<osg::FrameStamp> mEmitterStamp = new osg::FrameStamp;
        double mEmitterSeconds = 0.0;
        unsigned int mEmitterFrame = 0;

        /// Whether this walk is running emitters and looking through everything else.
        bool mStepOnly = false;

        /// The class the innermost root over the node being walked stated: everything under an
        /// actor's root is the actor. Carried down the subtree rather than read off each drawable,
        /// because the game marks the *root* and the drawables under it wear the masks they were
        /// authored with. Saved and restored around a descent, as `mPathHash` is.
        InstanceClass mClass = InstanceClass::Static;

        osg::Matrixf mRoot;
        std::size_t mFrame = 0;

        /// The last number this walk posed at, so a caller handing back a stale one is caught.
        unsigned int mTraversal = 0;

        /// The local-to-world of the node being visited, above `mRoot`.
        osg::Matrix mHere;

        /// The identity of the path the walk is standing on, saved and restored around each
        /// descent beside `mShading`. `identityWith` says what it is made of and why it is carried.
        std::size_t mPathHash = 0;

        /// The state sets in force where the walk is standing, nearest it last. Kept across walks
        /// and refilled, because a cell is tens of thousands of drawables and this is the frame
        /// path.
        std::vector<Shading> mShading;
    };

    MirrorTraversal::MirrorTraversal(SceneExtractor& extractor)
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        , mExtractor(extractor)
    {
        setFrameStamp(mStamp);
        mSequenceClock.setFrameStamp(mStamp);
    }

    void MirrorTraversal::begin(
        const osg::Matrixf& root, std::size_t frame, unsigned int traversal, std::size_t identity)
    {
        // The whole of what a traversal number promises. A state-set controller and an
        // `osg::Sequence` each keep the last number they ran at and do nothing for one they have
        // already seen, so a walk that handed back a number is a walk whose fires stand still — and
        // it fails as a frozen picture nobody can explain rather than as anything a log would carry.
        assert(traversal > mTraversal && "a mirror walk asked to run at a number it has already used");
        mTraversal = traversal;

        mRoot = root;
        mFrame = frame;
        mHere = osg::Matrix();
        mPathHash = identity;
        mShading.clear();

        // The mirror's own sequence and never the game's. What this walk runs — the controllers
        // and the sequences — is keyed on it, and a number taken from the game's frame would be a
        // second clock over the same nodes. `Traversals` is where that sequence lives and why there
        // is one of it.
        setTraversalNumber(traversal);
        mStamp->setFrameNumber(traversal);
    }

    void MirrorTraversal::apply(osg::Node& node)
    {
        enter(node, identityWith(mPathHash, &node));
    }

    void MirrorTraversal::enter(osg::Node& node, const std::size_t identity)
    {
        const NodeKind kind = mKinds.of(node);

        if (mStepOnly)
        {
            if (!stepParticles(node, kind))
                descend(node, kind);
            return;
        }

        // Told it was reached, because a semi-active skeleton stops moving its bones once three
        // traversals have passed with nothing reaching it, and here this walk is what reaches it.
        // The frame and not this walk's own number, because the update traversal is what compares.
        if (auto* skeleton = as<SceneUtil::Skeleton>(kind, NodeKind::Skeleton, node))
        {
            skeleton->markReached(static_cast<unsigned int>(mFrame));
        }
        else if (auto* source = as<SceneUtil::LightSource>(kind, NodeKind::LightSource, node))
        {
            mExtractor.addLight(*source, placed(), mStamp->getSimulationTime());
        }
        else if (stepParticles(node, kind))
        {
            // Neither of the two is a drawable or has a child, so there is no state set below them
            // to carry and nothing under them to reach.
            return;
        }

        const std::size_t held = mShading.size();
        const std::size_t above = mPathHash;
        mPathHash = identity;

        if (const osg::StateSet* own = node.getStateSet())
            pushShading(*own, false);

        // Above the node's own, which is where a rasterizing cull would push it too: what a
        // controller decided this frame overrides what the model was authored with.
        if (const osg::StateSet* animated = mExtractor.animate(node))
            pushShading(*animated, true);

        const InstanceClass outer = mClass;
        if (const std::optional<InstanceClass> stated = mExtractor.classOf(node.getNodeMask()))
            mClass = *stated;

        descend(node, kind);

        mClass = outer;
        mPathHash = above;
        mShading.resize(held);
    }

    /// Descends into the children of `node` that are in the world, and runs a flipbook's clock on
    /// the way past, because that clock lives in a traversal this renderer does not run:
    /// `SequenceClock` makes the claim it wants, and the frame it settled on is walked by the
    /// mirror. Unlike a particle step, a sequence step may be taken twice, because `Sequence`
    /// reads the simulation time outright. A branch that is off is off for its emitters too, and a
    /// system that comes back on after an hour is handed the hour in one step, as under a cull.
    void MirrorTraversal::descend(osg::Node& node, const NodeKind kind)
    {
        descendInWorld(node, kind, *this, [this](osg::Sequence& frames) { frames.traverse(mSequenceClock); });
    }

    /// Runs one node of an `osgParticle` simulation, and says whether that is what this node was.
    /// The whole of `osgParticle` hangs off the cull traversal — `ParticleProcessor::traverse` and
    /// `ParticleSystemUpdater::traverse` both open by asking whether the visitor is a cull visitor
    /// — and a ray tracer culls nothing, so this walk says it is one, to these two nodes and for
    /// the length of one call. Safe because neither casts: both only compare the type, and both
    /// derive from a plain `osg::Node` whose `traverse` is empty, so the claim cannot reach the
    /// three that would take it badly (`SceneUtil::RigGeometry`, `MorphGeometry`,
    /// `MWRender::CameraRelativeTransform`). This walk and not a cull of its own, because a
    /// processor reads its world transform off the visitor's node path.
    bool MirrorTraversal::stepParticles(osg::Node& node, const NodeKind kind)
    {
        if (auto* processor = as<osgParticle::ParticleProcessor>(kind, NodeKind::ParticleProcessor, node))
        {
            if (osgParticle::ParticleSystem* system = processor->getParticleSystem())
                keepRunning(*system);
        }
        else if (auto* updater = as<osgParticle::ParticleSystemUpdater>(kind, NodeKind::ParticleUpdater, node))
        {
            for (unsigned int at = 0; at < updater->getNumParticleSystems(); ++at)
                keepRunning(*updater->getParticleSystem(at));
        }
        else
            return false;

        // The emitter clock goes with the claim: what runs under it is the only thing in this walk
        // that must not be handed the world's.
        const osg::NodeVisitor::VisitorType was = getVisitorType();
        setVisitorType(CULL_VISITOR);
        setFrameStamp(mEmitterStamp);

        node.traverse(*this);

        setFrameStamp(mStamp);
        setVisitorType(was);

        return true;
    }

    void MirrorTraversal::advanceEmitters(double elapsed)
    {
        // The cap the game's own frame loop uses, and `MWRender::RainCounter` after it.
        constexpr double longest = 0.2;

        mEmitterSeconds += std::clamp(elapsed, 0.0, longest);
        mEmitterStamp->setSimulationTime(mEmitterSeconds);
        mEmitterStamp->setReferenceTime(mEmitterSeconds);
        mEmitterStamp->setFrameNumber(++mEmitterFrame);
    }

    void MirrorTraversal::stepOnly(osg::Node& node)
    {
        mStepOnly = true;
        node.accept(*this);
        mStepOnly = false;
    }

    /// Accumulated on the way down rather than recomputed on the way up: `osg::computeLocalToWorld`
    /// walks a drawable's whole path back to the root, O(depth) per drawable, and
    /// `computeLocalToWorldMatrix` is what it calls on each transform, so the answer is the same.
    /// The visitor goes with it and not the null pointer `computeLocalToWorld` passes, because the
    /// sky's `MWRender::CameraRelativeTransform` dereferences it without checking; a visitor that is
    /// not a cull visitor takes the branch a null one would have.
    void MirrorTraversal::enterTransform(osg::Transform& node, const std::size_t identity)
    {
        // Nothing an emitter needs is in the chain: a processor reads its world transform off the
        // node path, which `accept` keeps whatever this does.
        if (mStepOnly)
        {
            enter(node, identity);
            return;
        }

        const osg::Matrix above = mHere;
        node.computeLocalToWorldMatrix(mHere, this);

        enter(node, identity);

        mHere = above;
    }

    void MirrorTraversal::apply(osg::Transform& node)
    {
        enterTransform(node, identityWith(mPathHash, &node));
    }

    void MirrorTraversal::pushShading(const osg::StateSet& stateSet, const bool animated)
    {
        const float above = mShading.empty() ? 1.0f : mShading.back().mFade;
        mShading.push_back(Shading{
            .mStateSet = &stateSet,
            .mFade = fadeThrough(stateSet, above),
            .mAnimated = animated,
        });
    }

    void MirrorTraversal::apply(osg::Drawable& drawable)
    {
        if (mStepOnly)
            return;

        const std::size_t held = mShading.size();
        if (const osg::StateSet* own = drawable.getStateSet())
            pushShading(*own, false);

        mExtractor.addDrawable(drawable, identityWith(mPathHash, &drawable), mShading, placed(), mClass);

        mShading.resize(held);
    }

    /// Everything the content did not hide, asked of the loader that stamped the bit: a host that
    /// never configured `NifOsg::Loader` gets a mask of all ones and walks into nodes the content
    /// said are not there.
    SceneExtractor::SceneExtractor(SceneDesc& scene, Traversals* traversals)
        : mScene(scene)
        , mWalk(std::make_unique<MirrorTraversal>(*this))
        , mTraversals(traversals == nullptr ? mOwnTraversals : *traversals)
        , mTraversalMask(~NifOsg::Loader::getHiddenNodeMask())
    {
        // Reserved once, so no frame rehashes a map. A cell's drawables arriving grow every
        // identity map on that frame, and an `unordered_map` that grows past its buckets rehashes
        // on the insert that did it. Budgets past what a Morrowind exterior reaches at four cells
        // of distance, and a few hundred kilobytes of buckets apiece.
        mPlacements.reserve(sPlacementBudget);
        mMeshes.reserve(sMeshBudget, sDeformerBudget);
        mMaterials.reserve(sMaterialBudget, sTextureBudget, sAnimatedBudget);
        mEmitters.reserve(sEmitterBudget);
    }

    SceneExtractor::~SceneExtractor() = default;

    void SceneExtractor::setSimulationTime(double seconds)
    {
        osg::FrameStamp& stamp = mWalk->getStamp();
        stamp.setSimulationTime(seconds);
        stamp.setReferenceTime(seconds);
    }

    void SceneExtractor::advanceEmitters(double elapsed)
    {
        mWalk->advanceEmitters(elapsed);
    }

    void SceneExtractor::stepEmitters(osg::Node& node)
    {
        mWalk->stepOnly(node);
    }

    ExtractionStats SceneExtractor::extract(
        const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame)
    {
        return walk(node, transform, anchor, frame, {});
    }

    ExtractionStats SceneExtractor::extractWorld(
        const osg::Node& root, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame)
    {
        return walk(root, transform, anchor, frame, mResidents);
    }

    ExtractionStats SceneExtractor::walk(const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor,
        std::size_t frame, std::span<Residency* const> hidden)
    {
        ExtractionStats stats;
        mAnchor = anchor;
        mPass.mStats = &stats;

        mWalk->begin(transform, frame, mTraversals.next(), identitySeed(anchor));
        mWalk->setTraversalMask(mTraversalMask);

        // Non-const because the walk writes. It poses every actor it reaches and it runs every
        // state-set controller it finds, which is what makes an actor behind the camera posed and a
        // fire lit; OSG's visitor API is non-const regardless, so the cast happens once, here.
        const_cast<osg::Node&>(node).accept(*mWalk);

        // Inside the same walk, not beside it. What a residency stands is part of the same
        // frame as everything else — the same epoch, the same stats, the same sweep — and a second
        // `begin` would date it apart from the rest.
        for (Residency* resident : hidden)
            resident->collect(*this, stats);

        // After the whole walk, including whatever the residency brought in. Everything under it
        // has been stepped by now, so what the sprites are read from is a settled world rather than
        // one that depends on where an updater happened to sit among its siblings.
        mEmitters.flush();

        // So that a resolver reached outside a walk fails where it is, rather than counting into a
        // report that has gone.
        mPass.mStats = nullptr;

        return stats;
    }

    void SceneExtractor::take(osg::Node& node)
    {
        node.accept(*mWalk);
    }

    void SceneExtractor::advance()
    {
        mScene.placements().advance();
    }

    Retirement SceneExtractor::retire()
    {
        Retirement went;

        // Placements first, because dropping one is what makes its mesh droppable. Freed rather
        // than compacted, because a slot index is the custom index a hit reads back. Not run at all
        // where every placement was reached (`Kept::whole`), which is a world that stands still.
        mPlacements.retire([this](const Known& gone) { mScene.placements().drop(gone.mIndex); });

        // Both tables or neither, because `SceneDesc::release` frees them against one pair of
        // survivor lists and a list an earlier epoch filled names slots since handed out. Nothing at
        // all where both stand whole, which saves two walks of a map with one entry per drawable —
        // unless a hold on a row went to nought, which no map can see.
        if (!mMeshes.whole() || !mMaterials.whole() || mScene.hasDroppedHolds())
        {
            const std::size_t meshesBefore = mScene.meshes().getLiveCount();
            const std::size_t materialsBefore = mScene.materials().getLiveCount();

            mMeshes.retire(mLiveMeshes);
            mMaterials.retire(mLiveMaterials);

            // Freed, not compacted: closing the gaps would renumber every bottom-level acceleration
            // structure in the world on every crossing.
            mScene.release(mLiveMeshes, mLiveMaterials);

            // Counted off the tables rather than off the maps, because a row a hold let go of was in
            // no map to be counted there.
            went.mMeshes = static_cast<std::uint32_t>(meshesBefore - mScene.meshes().getLiveCount());
            went.mMaterials = static_cast<std::uint32_t>(materialsBefore - mScene.materials().getLiveCount());
        }

        // Swept whatever the two tables above did, because an image a material stopped reading, a
        // state set whose node left the graph and a sprite's texture each go stale on a frame where
        // no material died at all.
        mMeshes.retireDeformers();
        mMaterials.retireHolds();
        mEmitters.retire();

        // After the sweep and not before it, so that the walk which fills the next epoch is the
        // one this is measured against. Every entry that survived is still carrying the old stamp
        // and would be dropped on the spot otherwise.
        ++mPass.mEpoch;

        return went;
    }

    const osg::StateSet* SceneExtractor::animate(osg::Node& node)
    {
        return mMaterials.animate(node, mWalk.get());
    }

    void SceneExtractor::addLight(
        const SceneUtil::LightSource& source, const osg::Matrixf& place, double simulationTime)
    {
        // The recorded colours and this frame's scalars, never the colours the rasterizer draws
        // from (`lightColour`). `LightSource::getEmpty` is not asked: it means the model this light
        // hangs on has no geometry, which is a rasterizer's reason to skip a light, and a `LIGH`
        // whose mesh is empty still burns.
        const std::optional<Light> made
            = makeLight(lightColour(source, simulationTime), source.getSourceRadius(), place.getTrans());
        if (!made.has_value())
            return;

        mScene.addLight(*made);
        ++mPass.getStats().mLights;
    }

    void SceneExtractor::addDrawable(const osg::Drawable& drawable, const std::size_t who,
        const std::span<const Shading> shading, const osg::Matrixf& place, const InstanceClass what)
    {
        ExtractionStats& stats = mPass.getStats();

        // Asked before the geometry, because a particle system is an `osg::Drawable` with no
        // triangles in it at all: its sprites *are* the drawing, and they leave here as a run of
        // discs rather than as a mesh anything could build a structure over.
        const NodeKind kind = mKinds.of(drawable);
        if (const auto* particles = as<const osgParticle::ParticleSystem>(kind, NodeKind::ParticleSystem, drawable))
        {
            mEmitters.add(*particles, shading, place);
            return;
        }

        const DrawableRead read = readDrawable(drawable, kind);
        if (read.mGeometry == nullptr)
        {
            ++stats.mSkippedUnknown;
            return;
        }

        // Asked of the drawable and not of the path. OpenMW marks the water geometry itself, and
        // the node above it is a plain transform shared with anything else hanging there.
        const bool water = isWater(drawable.getNodeMask());

        // The material before the mesh, because a mesh records the material it arrives wearing.
        // `MeshRange::mMaterial` says why a static mesh has one to record; a backend bakes its mask
        // against that one, and the two counts past the mesh are what say the loader keeps it so.
        const MaterialResolver::Resolved material = water ? mMaterials.resolveWater() : mMaterials.resolve(shading);

        const Index mesh = mMeshes.resolve(drawable, read, material.mIndex);
        if (mesh == sNoIndex)
            return;

        // A placement wearing anything but the material its mesh arrived with is the canary —
        // `SceneUtil::CopyOp` shares the state set under every copy, so the only material a mesh
        // can be seen in two of is one a controller made per node.
        const Index arrivedWearing = mScene.meshes().getRows()[mesh].mMaterial;
        if (arrivedWearing != sNoIndex)
        {
            const Material& worn = mScene.materials().getRows()[arrivedWearing];
            if (!worn.mAnimated && material.mIndex != arrivedWearing)
                ++stats.mWornOtherwise;
        }

        // The slot this placement has held since it first appeared, so a world that stands
        // still writes nothing: the scene already knows where everything is, and only a transform
        // that differs from the one in the slot costs anything at all.
        const auto held = mPlacements.find(who);

        // Read for every surface and not for actors alone, because nothing here knows which is
        // which: what a mirror can see is a state set above this drawable that says how much of it
        // the game is showing, and the world's own answer to that is one.
        const float fade = shading.empty() ? 1.0f : shading.back().mFade;

        if (held == mPlacements.end())
        {
            const Index slot = mScene.addInstance(MeshInstance{
                .mTransform = place,
                .mMesh = mesh,
                .mMaterial = material.mIndex,
                .mOpacity = fade,
                .mClass = what,
            });

            mPlacements.add(who, Known{ .mIndex = slot });
        }
        else
        {
            mPlacements.stamp(held);
            mScene.placements().move(held->second.mIndex, place);
            mScene.placements().fade(held->second.mIndex, fade);
        }

        ++stats.mInstances;
    }

    bool SceneExtractor::isWater(osg::Node::NodeMask mask) const
    {
        return carriesOnly(mask, mWaterMask);
    }

    void SceneExtractor::setClassMask(const InstanceClass what, const osg::Node::NodeMask mask)
    {
        for (ClassMask& held : mClassMasks)
            if (held.mClass == what)
                held.mMask = mask;
    }

    std::optional<InstanceClass> SceneExtractor::classOf(const osg::Node::NodeMask mask) const
    {
        for (const ClassMask& held : mClassMasks)
            if (carriesOnly(mask, held.mMask))
                return held.mClass;

        return std::nullopt;
    }

    bool SceneExtractor::carriesOnly(osg::Node::NodeMask mask, osg::Node::NodeMask named)
    {
        // Every bit outside the named one, and not merely one inside it. A node mask is a
        // filter over passes and its default is all ones, so `mask & named` is true for every node
        // that never set one — which in this engine is nearly all of them, and would shade the whole
        // world as sea. What names the water, or the arms, is that no *other* pass may see it.
        return named != 0 && (mask & ~named) == 0;
    }

}
