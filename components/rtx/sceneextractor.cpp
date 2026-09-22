#include "sceneextractor.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <osg/FrameStamp>
#include <osg/Matrix>
#include <osg/NodeVisitor>
#include <osg/Sequence>
#include <osg/ref_ptr>
#include <osgParticle/Particle>
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>

#include <components/nifosg/autotransform.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/stableidentity.hpp>

#include "cellring.hpp"
#include "lightbuilder.hpp"
#include "material.hpp"
#include "meshreader.hpp"
#include "mirroridentity.hpp"
#include "nodekind.hpp"
#include "runs.hpp"
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

        /// Effects a walk can be handed before its list of glows grows: every bolt in the air,
        /// every burst and every cast, which a fight of a dozen casters does not reach.
        constexpr std::size_t sEffectBudget = 256;

        /// What identifies one placement from one frame to the next: the anchor a walk starts from
        /// and the node's place under it, folded on the way down so the prefix every sibling shares
        /// is worked out once. The place is structural — which child of which child — from the
        /// nearest node the engine stamped a `SceneUtil::StableIdentity` on, and that stamp
        /// restarts the fold: a reference root is known by what it stands and not by what is
        /// stood before it in the cell. A hundred crates share one geometry and, walked from a
        /// shared template node, one structure as well, which the anchor and the stamps tell apart.
        /// Nothing here reads an address, so nothing here has to be kept alive to keep it true.
        std::size_t identityWith(std::size_t key, const std::size_t part)
        {
            return (key ^ part) * 0x100000001b3ull;
        }

        std::size_t identitySeed(std::size_t anchor)
        {
            return identityWith(0xcbf29ce484222325ull, anchor);
        }

        /// A stamped node's identity, in a fold of its own so a stamp cannot collide with a
        /// structural place under the same seed.
        std::size_t identityStamped(std::size_t seed, const std::uint64_t id)
        {
            return identityWith(identityWith(seed, 0x9e3779b97f4a7c15ull), static_cast<std::size_t>(id));
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
        MirrorTraversal(SceneExtractor& extractor, const NodeKinds& kinds);

        /// Points the walk at a root, at where it stands, and at the frame it is mirroring.
        void begin(const osg::Matrixf& root, std::size_t frame, unsigned int traversal, std::size_t identity);

        osg::FrameStamp& getStamp() { return *mStamp; }

        /// Moves the emitter clock on by one frame. See `mEmitterStamp`.
        void advanceEmitters(double elapsed);

        void apply(osg::Node& node) override;
        void apply(osg::Transform& node) override;
        void apply(osg::Drawable& drawable) override;

    private:
        /// What `node` is known as: its stamp under the walk's seed, or its place under its parent.
        /// A stamp is looked for down to `SceneExtractor::setStampDepth` and no deeper, because the
        /// look is a load off a line of the node the walk does not otherwise touch, and a walk is
        /// fifty thousand nodes.
        std::size_t identityOf(const osg::Node& node) const;

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

        /// The extractor's own classifier, which answers for a node here and for a drawable there:
        /// one table, because the answers are a fact about the classes in the world, and both are
        /// asked on the one thread that walks.
        const NodeKinds& mKinds;

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

        /// The walk's seed, which a stamped node's identity restarts from.
        std::size_t mSeed = 0;

        /// The identity of the path the walk is standing on, saved and restored around each
        /// descent beside `mShading`. `identityWith` says what it is made of and why it is carried.
        std::size_t mPathHash = 0;

        /// Which child of its parent the node being entered is. Set by the loop that descends,
        /// which is why every descent here is a loop of this walk's own and never `traverse`.
        unsigned int mChildIndex = 0;

        /// How many nodes are above the one being entered; nought at the root.
        unsigned int mDepth = 0;

        /// `SceneExtractor::getStampDepth`, read once at `begin`.
        unsigned int mStampDepth = 0;

        /// The state sets in force where the walk is standing, nearest it last. Kept across walks
        /// and refilled, because a cell is tens of thousands of drawables and this is the frame
        /// path.
        std::vector<Shading> mShading;
    };

    MirrorTraversal::MirrorTraversal(SceneExtractor& extractor, const NodeKinds& kinds)
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        , mExtractor(extractor)
        , mKinds(kinds)
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
        mSeed = identity;
        mPathHash = identity;
        mChildIndex = 0;
        mDepth = 0;
        mStampDepth = mExtractor.getStampDepth();
        mShading.clear();

        // The mirror's own sequence and never the game's. What this walk runs — the controllers
        // and the sequences — is keyed on it, and a number taken from the game's frame would be a
        // second clock over the same nodes. `Traversals` is where that sequence lives and why there
        // is one of it.
        setTraversalNumber(traversal);
        mStamp->setFrameNumber(traversal);
    }

    std::size_t MirrorTraversal::identityOf(const osg::Node& node) const
    {
        if (mDepth <= mStampDepth)
            if (const SceneUtil::StableIdentity* stamped = SceneUtil::StableIdentity::find(node))
                return identityStamped(mSeed, stamped->getId());

        return identityWith(mPathHash, mChildIndex);
    }

    void MirrorTraversal::apply(osg::Node& node)
    {
        enter(node, identityOf(node));
    }

    void MirrorTraversal::enter(osg::Node& node, const std::size_t identity)
    {
        const NodeKind kind = mKinds.of(node);

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

        ++mDepth;

        if (const osg::StateSet* own = node.getStateSet())
            pushShading(*own, false);

        // Above the node's own, which is where a rasterizing cull would push it too: what a
        // controller decided this frame overrides what the model was authored with.
        if (const osg::StateSet* animated = mExtractor.animate(node, animatedThrough(mShading)))
            pushShading(*animated, true);

        const InstanceClass outer = mClass;
        if (const std::optional<InstanceClass> stated = mExtractor.classOf(node.getNodeMask()))
            mClass = *stated;

        // The root of a magic effect, whose sheets and flames light the world as one lamp: opened
        // here and closed on the way back up, once everything under it has been placed.
        const bool glows = mClass == InstanceClass::Effect && outer != InstanceClass::Effect;
        if (glows)
            mExtractor.openGlow();

        descend(node, kind);

        if (glows)
            mExtractor.closeGlow();

        mClass = outer;
        mPathHash = above;
        --mDepth;
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
        descendInWorld(
            node, kind, *this, [this](osg::Sequence& frames) { frames.traverse(mSequenceClock); },
            [this](const unsigned int child) { mChildIndex = child; });
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

        // `traverse` and not a loop over children, because the processor's and the updater's own
        // `traverse` is where they step: neither has a child to hand an index.
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

    /// Accumulated on the way down rather than recomputed on the way up: `osg::computeLocalToWorld`
    /// walks a drawable's whole path back to the root, O(depth) per drawable, and
    /// `computeLocalToWorldMatrix` is what it calls on each transform, so the answer is the same.
    /// The visitor goes with it and not the null pointer `computeLocalToWorld` passes, because the
    /// sky's `MWRender::CameraRelativeTransform` dereferences it without checking; a visitor that is
    /// not a cull visitor takes the branch a null one would have.
    void MirrorTraversal::enterTransform(osg::Transform& node, const std::size_t identity)
    {
        const osg::Matrix above = mHere;

        // **A billboard is turned here, toward the eye this walk was told**, because the node
        // turns only under a cull visitor and this walk is none: handed itself, `computeMatrix`
        // keeps whatever rotation a cull last left, which in this renderer is the one the file was
        // authored with. The three vectors go in the node's own frame, which is what a cull stack
        // hands it too. A walk told no eye leaves the billboard where it stands.
        const std::optional<ViewBasis>& eye = mExtractor.getEye();
        if (auto* billboard = as<NifOsg::AutoTransform>(mKinds.of(node), NodeKind::Billboard, node);
            billboard != nullptr && eye.has_value())
        {
            const osg::Matrixd toLocal = osg::Matrixd::inverse(osg::Matrixd(above) * osg::Matrixd(mRoot));
            const osg::Vec3d eyeLocal = osg::Vec3d(eye->mOrigin) * toLocal;
            const osg::Vec3d lookLocal = osg::Matrixd::transform3x3(osg::Vec3d(eye->mForward), toLocal);
            const osg::Vec3d upLocal = osg::Matrixd::transform3x3(osg::Vec3d(eye->mUp), toLocal);

            mHere.preMult(billboard->computeMatrixForFrame(eyeLocal, lookLocal, upLocal));
        }
        else
            node.computeLocalToWorldMatrix(mHere, this);

        enter(node, identity);

        mHere = above;
    }

    void MirrorTraversal::apply(osg::Transform& node)
    {
        enterTransform(node, identityOf(node));
    }

    void MirrorTraversal::pushShading(const osg::StateSet& stateSet, const bool animated)
    {
        const Shading* const above = mShading.empty() ? nullptr : &mShading.back();
        mShading.push_back(Shading{
            .mStateSet = &stateSet,
            .mFade = fadeThrough(stateSet, above != nullptr ? above->mFade : 1.0f),
            .mAnimated = animated,
            .mAnimatedThrough = animated || (above != nullptr && above->mAnimatedThrough),
        });
    }

    void MirrorTraversal::apply(osg::Drawable& drawable)
    {
        const std::size_t held = mShading.size();
        if (const osg::StateSet* own = drawable.getStateSet())
        {
            pushShading(*own, false);

            // A drawable carries no controller of its own, so what this asks is the other half of
            // `animate`: a state set of its own under an animated one.
            if (const osg::StateSet* animated = mExtractor.animate(drawable, animatedThrough(mShading)))
                pushShading(*animated, true);
        }

        mExtractor.addDrawable(drawable, identityWith(mPathHash, mChildIndex), mShading, placed(), mClass);

        mShading.resize(held);
    }

    /// Every node, until the owner states a mask — `setTraversalMask`, which says why the default
    /// is not the narrower answer it looks like it should be.
    SceneExtractor::SceneExtractor(SceneDesc& scene, Traversals* traversals)
        : mScene(scene)
        , mWalk(std::make_unique<MirrorTraversal>(*this, mKinds))
        , mTraversals(traversals == nullptr ? mOwnTraversals : *traversals)
        , mTraversalMask(~0u)
    {
        // Reserved once, so no frame rehashes a map. A cell's drawables arriving grow every
        // identity map on that frame, and an `unordered_map` that grows past its buckets rehashes
        // on the insert that did it. Budgets past what a Morrowind exterior reaches at four cells
        // of distance, and a few hundred kilobytes of buckets apiece.
        mPlacements.reserve(sPlacementBudget);
        mMeshes.reserve(sMeshBudget, sDeformerBudget);
        mMaterials.reserve(sMaterialBudget, sTextureBudget, sAnimatedBudget);
        mEmitters.reserve(sEmitterBudget);
        mGlows.reserve(sEffectBudget);
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

    ExtractionStats SceneExtractor::extract(
        const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame)
    {
        return walk(node, transform, anchor, frame, nullptr, false);
    }

    ExtractionStats SceneExtractor::extractWorld(
        const osg::Node& root, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame, CellRing& ring)
    {
        // Here, because this is the one call that holds the ring and the frame both: a ring told
        // one frame and walked for another adopts twice on a frame walked twice.
        ring.setFrame(frame);

        return walk(root, transform, anchor, frame, &ring, false);
    }

    ExtractionStats SceneExtractor::extractFalling(
        const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame)
    {
        return walk(node, transform, anchor, frame, nullptr, true);
    }

    SceneExtractor::WalkGuard::WalkGuard(
        MirrorPass& pass, Stepped<Phase>& phase, ExtractionStats& stats, const bool falls)
        : mPass(pass)
        , mPhase(phase)
    {
        mPhase.step(Phase::Walking, Phase::Between);
        mPass.mStats = &stats;
        mPass.mFalls = falls;
    }

    SceneExtractor::WalkGuard::~WalkGuard()
    {
        // So that a resolver reached outside a walk fails where it is, rather than counting into a
        // report that has gone.
        mPass.mStats = nullptr;
        mPass.mFalls = false;
        mPhase.step(Phase::Between, Phase::Walking);
    }

    ExtractionStats SceneExtractor::walk(const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor,
        std::size_t frame, CellRing* const ring, const bool falls)
    {
        ExtractionStats stats;
        const WalkGuard walking(mPass, mPhase, stats, falls);

        // Before anything is placed, so a walk that threw has still marked the scene: what it
        // stamped before the throw is standing, and the sweep is owed for the rest.
        mScene.noteWalked();

        mWalk->begin(transform, frame, mTraversals.next(), identitySeed(anchor));
        mWalk->setTraversalMask(mTraversalMask);

        // An effect a walk that threw was inside is not one this walk is inside, and its glows
        // were never made.
        mGlow.reset();
        mGlows.clear();

        // Non-const because the walk writes. It poses every actor it reaches and it runs every
        // state-set controller it finds, which is what makes an actor behind the camera posed and a
        // fire lit; OSG's visitor API is non-const regardless, so the cast happens once, here.
        const_cast<osg::Node&>(node).accept(*mWalk);

        // Inside the same walk, not beside it. What the ring stands is part of the same frame as
        // everything else — the same epoch, the same stats, the same sweep — and a second `begin`
        // would date it apart from the rest.
        if (ring != nullptr)
            ring->collect(*this, stats);

        // After the whole walk, including whatever the ring brought in. Everything under it
        // has been stepped by now, so what the sprites are read from is a settled world rather than
        // one that depends on where an updater happened to sit among its siblings.
        mEmitters.flush(mGlows);

        // And the effects' lamps after the emitters, because a burst's flames are in them.
        for (const Glow& glow : mGlows)
        {
            if (const std::optional<Light> made = glow.makeLight(); made.has_value())
            {
                mScene.addLight(*made);
                ++stats.mLights;
            }
        }

        return stats;
    }

    Retirement SceneExtractor::detach(CellRing& ring)
    {
        mPhase.expect(Phase::Between);

        // Moved on before anything is let go of, so that a hold dropped here is dropped off an
        // entry of an earlier epoch and owes the sweep, and the sweep keeps nothing stamped: what
        // this epoch reached is nothing.
        ++mPass.mEpoch;
        ring.releaseHolds(*this);

        return retire();
    }

    Retirement SceneExtractor::retire()
    {
        mPhase.expect(Phase::Between);

        Retirement went;

        // Placements first, because dropping one is what makes its mesh droppable. Freed rather
        // than compacted, because a slot index is the custom index a hit reads back. Not run at all
        // where every placement was reached (`Kept::whole`), which is a world that stands still.
        mPlacements.retire([this](const Known& gone) { mScene.placements().drop(gone.mIndex, Stander::Walk); });

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

        // What stands is what the walks met, which is what lets the scene be handed over.
        mScene.noteSwept();

        // Every live texture and deformer is held and every placement stands on live rows: what
        // the sweeps above leave true, asked here because a slot taken and never held is one no
        // sweep can reach, and this is the one point every frame passes after them.
        assert(mScene.isConsistent() && "a retire left a live row nothing holds, or a placement on a freed one");

        return went;
    }

    const osg::StateSet* SceneExtractor::animate(osg::Node& node, const bool underAnimated)
    {
        return mMaterials.animate(node, mWalk.get(), underAnimated);
    }

    void SceneExtractor::addLight(
        const SceneUtil::LightSource& source, const osg::Matrixf& place, double simulationTime)
    {
        // The recorded colours and this frame's scalars, never the colours the rasterizer draws
        // from (`lightColour`). `LightSource::getEmpty` is not asked: it means the model this light
        // hangs on has no geometry, which is a rasterizer's reason to skip a light, and a `LIGH`
        // whose mesh is empty still burns.
        const osg::Vec3f colour = lightColour(source, simulationTime);

        // The radius the content states, and not the cut-off the rasterizer widened it to: a
        // bolt's is its spell's area, which `ProjectileManager` writes there.
        const float radius = source.getSourceRadius();
        const std::optional<Light> made
            = isFill(source) ? makeFill(colour, radius, place.getTrans()) : makeLight(colour, radius, place.getTrans());
        if (!made.has_value())
            return;

        // A light the game hung on an effect is the effect's light — `Glow::mLit`. Whether the
        // light stood before or after the sheets under the same root does not matter, because
        // the glow is made after the walk.
        if (mGlow.has_value())
            mGlows[*mGlow].mLit = true;

        mScene.addLight(*made);
        ++mPass.getStats().mLights;
    }

    void SceneExtractor::openGlow()
    {
        assert(!mGlow.has_value() && "a walk entered an effect while inside one");
        mGlow = mGlows.size();
        mGlows.emplace_back();
    }

    void SceneExtractor::closeGlow()
    {
        assert(mGlow.has_value() && "a walk left an effect it never entered");
        mGlow.reset();
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
            mEmitters.add(*particles, shading, place, mGlow);
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

        const MaterialResolver::Resolved material = water ? mMaterials.resolveWater() : mMaterials.resolve(shading);

        const Index mesh = mMeshes.resolve(drawable, read);
        if (mesh == sNoIndex)
            return;

        // The slot this placement has held since it first appeared, so a world that stands
        // still writes nothing: the scene already knows where everything is, and only a transform
        // that differs from the one in the slot costs anything at all.
        const auto held = mPlacements.find(who);

        // Read for every surface and not for actors alone, because nothing here knows which is
        // which: what a mirror can see is a state set above this drawable that says how much of it
        // the game is showing, and the world's own answer to that is one.
        const float fade = shading.empty() ? 1.0f : shading.back().mFade;

        const MeshInstance resolved{
            .mTransform = place,
            .mMesh = mesh,
            .mMaterial = material.mIndex,
            .mOpacity = fade,
            .mClass = what,
        };

        ++stats.mInstances;

        // What the sheet adds to the effect's lamp, read off the rows the placement stands on
        // this frame: a controller may have rewritten the material on the way here, and
        // `resolve` rewrote the row before this read.
        if (mGlow.has_value() && material.mIndex != sNoIndex)
            mGlows[*mGlow].addSheet(
                mScene.materials().getRows()[material.mIndex], mScene.meshes().getRows()[mesh].mBounds, place, fade);

        if (held == mPlacements.end())
        {
            mPlacements.add(who, Known{ .mIndex = mScene.addInstance(resolved) });
            return;
        }

        mPlacements.stamp(held);

        // A slot stands what the walk resolved this frame, or it is stood again: a deforming
        // drawable whose source geometry was replaced, a state set a controller rewrote into
        // another material, or a sibling that shifted into this place when the one before it went,
        // is mirrored afresh under the identity it kept. Moved, the slot would carry the old
        // surface at the new place until the next sweep, standing on a row the sweep may free.
        Index& slot = held->second.mIndex;
        const MeshInstance& standing = mScene.placements().getRows()[slot].mInstance;
        if (standing.mMesh != resolved.mMesh || standing.mMaterial != resolved.mMaterial
            || standing.mClass != resolved.mClass)
        {
            mScene.placements().drop(slot, Stander::Walk);
            slot = mScene.addInstance(resolved);
            ++stats.mRestood;
            return;
        }

        mScene.placements().move(slot, place);
        mScene.placements().fade(slot, fade);
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
