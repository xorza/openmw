#include "sceneextractor.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "lightbuilder.hpp"
#include "nodelibrary.hpp"

#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/Sequence>
#include <osg/Switch>
#include <osgParticle/Particle>
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>

#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <span>

#include <components/nifosg/nifloader.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/skeleton.hpp>
// `terraindrawable.hpp` holds `osg::ref_ptr`s to composite-map types it only forward-declares, so it
// does not compile on its own. This is what completes them.
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

namespace Rtx
{
    namespace
    {
        /// Clears the one gate a renderer with no draw can only ever answer wrongly.
        ///
        /// `osgParticle` stops a system whose draw has not touched it for two frames — a sound
        /// saving when the draw is what advances that number, and a permanent stop when nothing
        /// draws through OpenGL at all. `ParticleSystem::_last_frame` moves in `drawImplementation`
        /// and nowhere else, so two frames in, every system in the world is judged off screen and
        /// stopped for good.
        ///
        /// Said where the system is driven rather than where its sprites are read, because it is
        /// true of every system this renderer runs and not only of the ones with a texture to show.
        void keepRunning(osgParticle::ParticleSystem& system)
        {
            if (system.getFreezeOnCull())
                system.setFreezeOnCull(false);
        }

        /// What identifies one placement from one frame to the next: the seed a walk starts from,
        /// and the fold each node of its path adds.
        ///
        /// **The anchor and the node path under it, together.** Neither is enough alone: a drawable
        /// is not an instance, because a hundred crates share one geometry, and a path is not one
        /// either, because a hundred crates walked from a shared template node share the path as
        /// well. What tells them apart is what the caller was placing.
        ///
        /// Hashed rather than kept, because a path is a vector of pointers per placement and the map
        /// is walked every frame; at sixty-four bits over tens of thousands of placements a collision
        /// is not a thing that happens.
        ///
        /// **Folded on the way down rather than taken at the leaf**, which is the argument `mHere`
        /// makes for the matrix: the prefix every sibling under a node shares is worked out once as
        /// the walk enters that node, against a depth's worth per drawable.
        std::size_t identitySeed(std::size_t anchor)
        {
            return (0xcbf29ce484222325ull ^ anchor) * 0x100000001b3ull;
        }

        std::size_t identityWith(std::size_t key, const osg::Node* node)
        {
            return (key ^ std::hash<const osg::Node*>{}(node)) * 0x100000001b3ull;
        }

        /// The texture bound at `unit`, or null.
    }

    /// Walks the graph and hands every geometry it meets to the extractor.
    /// Runs an `osg::Sequence`'s clock, and reaches nothing. See `MirrorTraversal::descend`.
    ///
    /// **A visitor of its own, because the claim it makes is one the mirror may not carry.**
    /// `Sequence::traverse` moves its clock only for a visitor that says it is an update traversal
    /// *and* walks in `TRAVERSE_ACTIVE_CHILDREN`; either one alone leaves the frame number at -1 and
    /// nothing is shown at all. Neither claim is true of the mirror.
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

    class MirrorTraversal : public osg::NodeVisitor
    {
    public:
        explicit MirrorTraversal(SceneExtractor& extractor);

        /// Points the walk at a root, at where it stands, and at the frame it is mirroring.
        void begin(const osg::Matrixf& root, std::size_t frame, unsigned int traversal, std::size_t identity,
            ExtractionStats& stats);

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
        /// Descends into the children of `node` that are in the world. See below.
        void descend(osg::Node& node);

        /// Puts `stateSet` at the near end of the chain, with the fade resolved through it.
        void pushShading(const osg::StateSet& stateSet, bool animated);

        /// Runs one node of an `osgParticle` simulation, if that is what this node is. See below.
        bool stepParticles(osg::Node& node);

        /// Where the node being visited stands in the world.
        ///
        /// Narrowed to single precision here and not before: `mHere` accumulates in the width
        /// `computeLocalToWorld` returned, so a placement lands on the bits it landed on when every
        /// drawable worked the chain out for itself.
        osg::Matrixf placed() const { return osg::Matrixf(mHere) * mRoot; }

        SceneExtractor& mExtractor;

        /// The clock every controller under this walk reads. Its simulation time is the world's;
        /// its frame number is the walk's own, for the reason `begin` gives.
        osg::ref_ptr<osg::FrameStamp> mStamp = new osg::FrameStamp;

        /// A member for the reason the walk is: made once, and a frame allocates none of it.
        SequenceClock mSequenceClock;

        /// **The emitters' own clock, and it is not the world's.**
        ///
        /// `osgParticle` integrates the difference between one frame stamp and the last, so what it
        /// is handed has to move forward in the steps its content was authored against. A step of
        /// nothing emits nothing; a step of half an hour puts every plume in the cell on its own
        /// ceiling at once — and the world's clock does both, across a loading screen, a paused
        /// window, or a harness warming its emitters while the world holds still. This one moves
        /// only through `advanceEmitters`, which cannot be handed a jump or a step backwards.
        ///
        /// Its frame number is the one sequence `ParticleProcessor` keeps its once-per-frame guard
        /// against, so however many walks reach an emitter, exactly one of them steps it — and that
        /// is why nothing else in this renderer may drive a particle system. Two clocks writing that
        /// guard is not two steps, it is a `_t0` from whichever wrote last.
        osg::ref_ptr<osg::FrameStamp> mEmitterStamp = new osg::FrameStamp;
        double mEmitterSeconds = 0.0;
        unsigned int mEmitterFrame = 0;

        /// Whether this walk is running emitters and looking through everything else.
        bool mStepOnly = false;

        /// How many first-person roots stand over the node being walked: everything under one is
        /// the player's own arms. Counted down the subtree rather than read off each drawable,
        /// because the game marks the *root* and the drawables under it wear the masks they were
        /// authored with.
        unsigned int mFirstPerson = 0;

        osg::Matrixf mRoot;
        std::size_t mFrame = 0;

        /// The last number this walk posed at, so a caller handing back a stale one is caught.
        unsigned int mTraversal = 0;
        ExtractionStats* mStats = nullptr;

        /// The local-to-world of the node being visited, above `mRoot`.
        osg::Matrix mHere;

        /// The identity of the path the walk is standing on, saved and restored around each
        /// descent beside `mShading`. `identitySeed` says what it is made of and why it is carried.
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

    void MirrorTraversal::begin(const osg::Matrixf& root, std::size_t frame, unsigned int traversal,
        std::size_t identity, ExtractionStats& stats)
    {
        // **The whole of what a traversal number promises.** A state-set controller and an
        // `osg::Sequence` each keep the last number they ran at and do nothing for one they have
        // already seen, so a walk that handed back a number is a walk whose fires stand still — and
        // it fails as a frozen picture nobody can explain rather than as anything a log would carry.
        assert(traversal > mTraversal && "a mirror walk asked to run at a number it has already used");
        mTraversal = traversal;

        mRoot = root;
        mFrame = frame;
        mStats = &stats;
        mHere = osg::Matrix();
        mPathHash = identity;
        mShading.clear();

        // **The mirror's own sequence and never the game's.** What this walk runs — the controllers
        // and the sequences — is keyed on it, and a number taken from the game's frame would be a
        // second clock over the same nodes. `Traversals` is where that sequence lives and why there
        // is one of it.
        setTraversalNumber(traversal);
        mStamp->setFrameNumber(traversal);
    }

    void MirrorTraversal::apply(osg::Node& node)
    {
        if (mStepOnly)
        {
            if (!stepParticles(node))
                descend(node);
            return;
        }

        // **The two node types this looks at rather than through**, split on `asGroup` so that
        // neither pays for the other's cast: a light is an `osg::Node` and a skeleton is an
        // `osg::Group`, so one question answers which of the two a node could be.
        if (osg::Group* group = node.asGroup())
        {
            // **Told it was reached, because nothing else here will tell it.** A semi-active
            // skeleton — which is every actor but the player — skips its update traversal, and so
            // stops moving its bones, once three traversals have passed with nothing reaching it.
            // Under a renderer that culls, the cull is what keeps saying so. This walk is what
            // reaches an actor here, so this walk is what says so.
            //
            // **The frame and not this walk's own number.** What compares against it is the update
            // traversal, whose number is the frame's; a pose number is a different sequence that
            // only agrees with it by accident. It agreed on the ship at a new game, where the first
            // walk happens on the first frame, and was twelve behind after a savegame load — where
            // the loading screen's frames are updates with no walk between them — which froze every
            // actor in the world and left them sliding about in the pose they arrived in.
            //
            // **Gated on the library before the cast**, here and below. Both classes are
            // `SceneUtil`'s, and a node from `osg` or `NifOsg` — which is nearly every node in a
            // cell — answers the gate in a byte where a failed `dynamic_cast` walks the class
            // hierarchy to say the same thing.
            if (auto* skeleton = isFrom(node, "SceneUtil") ? dynamic_cast<SceneUtil::Skeleton*>(group) : nullptr)
                skeleton->markReached(static_cast<unsigned int>(mFrame));
        }
        else if (auto* source = isFrom(node, "SceneUtil") ? dynamic_cast<SceneUtil::LightSource*>(&node) : nullptr)
        {
            mExtractor.addLight(*source, getNodePath(), placed(), mStamp->getSimulationTime(), *mStats);
        }
        else if (stepParticles(node))
        {
            // Neither of the two is a drawable or has a child, so there is no state set below them
            // to carry and nothing under them to reach.
            return;
        }

        const std::size_t held = mShading.size();
        const std::size_t above = mPathHash;
        mPathHash = identityWith(mPathHash, &node);

        if (const osg::StateSet* own = node.getStateSet())
            pushShading(*own, false);

        // Above the node's own, which is where a rasterizing cull would push it too: what a
        // controller decided this frame overrides what the model was authored with.
        if (const osg::StateSet* animated = mExtractor.animate(node))
            pushShading(*animated, true);

        const unsigned int arms = mExtractor.isFirstPerson(node.getNodeMask()) ? 1u : 0u;
        mFirstPerson += arms;

        descend(node);

        mFirstPerson -= arms;
        mPathHash = above;
        mShading.resize(held);
    }

    /// Descends into the children of `node` that are in the world.
    ///
    /// **Three node types in this tree choose among their children, and they get three answers.**
    /// A switch is honoured. A sequence is honoured *and stepped*. An LOD is not honoured at all,
    /// because a ray is owed the finest child a node has rather than the one a distance test picked
    /// for an eye. That is the whole of the decision, and it is why the walk stays in
    /// `TRAVERSE_ALL_CHILDREN`: the one mode that would answer the first two also answers the third,
    /// and it answers it wrongly.
    ///
    /// **`osg::Switch`**: its `traverse` visits every child under `TRAVERSE_ALL_CHILDREN`, so a
    /// branch that is switched off is mirrored anyway. `MWRender`'s `DayNightCallback` leaves the
    /// night lamp traced at noon and the day mesh traced at midnight, both at once, and a harvested
    /// plant is traced through the unharvested one it replaced. This is geometry and not only light.
    ///
    /// **`osg::Sequence`**: `NifOsg` builds one for every `NiFltAnimationNode`, which is Morrowind's
    /// flipbook — a fire, a forge, a lava flow. Under `TRAVERSE_ALL_CHILDREN` every frame of it is
    /// traced at once and in the same place, and its clock never moves. Stepping it here is the same
    /// statement `stepParticles` makes below: the clock lives in a traversal this renderer does not
    /// run, so this walk is what has to run it. `SequenceClock` is what makes the claim that clock
    /// wants, and then the frame it settled on is walked by the mirror itself — measured, because
    /// handing `Sequence::traverse` only the traversal mode leaves its frame at -1 and shows nothing.
    ///
    /// **Unlike a particle step, a sequence step may be taken twice.** `Sequence` reads the frame
    /// stamp's simulation time outright, so two calls at the same time settle on the same frame —
    /// which is what makes it safe on the `stepOnly` pass as well as on a mirrored frame.
    ///
    /// A branch that is off is off for its emitters too, and an `osgParticle` step is the difference
    /// between one frame stamp and the last one that reached it — so a system that comes back on
    /// after an hour is handed the hour in one step. That is what the rasterizer's cull does with the
    /// same graph, and it is a property of `osgParticle`'s clock rather than of this walk.
    void MirrorTraversal::descend(osg::Node& node)
    {
        if (osg::Switch* branches = node.asSwitch())
        {
            for (unsigned int at = 0; at < branches->getNumChildren(); ++at)
                if (branches->getValue(at))
                    branches->getChild(at)->accept(*this);

            return;
        }

        // Cast the group and not the node: this walk reaches far more drawables than groups, and
        // only a group can be a sequence. And the class before the cast: `osg` is every plain
        // group in a cell, and nothing derives from `Sequence`.
        if (auto* frames
            = std::strcmp(node.className(), "Sequence") == 0 ? dynamic_cast<osg::Sequence*>(node.asGroup()) : nullptr)
        {
            frames->traverse(mSequenceClock);

            const int shown = frames->getValue();
            if (shown >= 0 && shown < static_cast<int>(frames->getNumChildren()))
                frames->getChild(shown)->accept(*this);

            return;
        }

        traverse(node);
    }

    /// Runs one node of an `osgParticle` simulation, and says whether that is what this node was.
    ///
    /// **The whole of `osgParticle` hangs off the cull traversal.** Emission, the affector programs
    /// and the integration all live in `ParticleProcessor::traverse` and
    /// `ParticleSystemUpdater::traverse`, and both open by asking whether the visitor calling them
    /// is a cull visitor. A ray tracer culls nothing, so left alone every particle system in the
    /// world stands still on the state its file was authored with: candles without flames, braziers
    /// without smoke, rain that never falls. This walk is the only thing that reaches them, so this
    /// walk is what runs them — the same reason it is what poses an actor.
    ///
    /// **So it says it is a cull visitor, to these two nodes and for the length of one call.**
    /// `PoseCull` is a real one and warns against exactly this, because `SceneUtil::RigGeometry`
    /// and `MorphGeometry` answer the same question with an unchecked `static_cast` — as does
    /// `MWRender::CameraRelativeTransform`, which `apply(osg::Transform&)` below hands this very
    /// visitor. Neither of these two casts: both only compare the type, and
    /// `ParticleSystem::update` reaches for the visitor through `asCullVisitor`, which answers null
    /// and skips a depth sort a ray tracer has no use for. Both derive from a plain `osg::Node`,
    /// whose `traverse` is empty, so the claim cannot reach a child — and that is what keeps it
    /// away from the three that would take it badly.
    ///
    /// This walk and not a cull of its own, for the same reason it is here at all: a processor reads
    /// its world transform off the visitor's node path, and this is the walk standing on one.
    bool MirrorTraversal::stepParticles(osg::Node& node)
    {
        // The two libraries a processor or an updater can come from: `osgParticle`'s own, and
        // `NifOsg::Emitter` over them. A node from anywhere else fails both casts below.
        if (!isFrom(node, "osgParticle") && !isFrom(node, "NifOsg"))
            return false;

        if (auto* processor = dynamic_cast<osgParticle::ParticleProcessor*>(&node))
        {
            if (osgParticle::ParticleSystem* system = processor->getParticleSystem())
                keepRunning(*system);
        }
        else if (auto* updater = dynamic_cast<osgParticle::ParticleSystemUpdater*>(&node))
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

    /// **Accumulated on the way down rather than recomputed on the way up.**
    ///
    /// `osg::computeLocalToWorld` walks a drawable's whole path back to the root and multiplies the
    /// chain again, so a product every sibling under a transform shares is rebuilt once per sibling
    /// — O(depth) per drawable, in a visitor already standing at that depth. One multiply per
    /// transform *entered* is the same answer for a fraction of the work, and on a nine-by-nine
    /// exterior it is the difference between 47,828 chain walks a frame and about a tenth as many
    /// matrix multiplies.
    ///
    /// `computeLocalToWorldMatrix` is what `computeLocalToWorld` calls on each transform it meets,
    /// so the answer is the same one: an absolute reference frame still replaces the accumulation
    /// instead of adding to it, because that is the branch inside it that does so.
    ///
    /// **The visitor goes with it, and not the null pointer `computeLocalToWorld` passes.** That
    /// function only ever reaches a transform with a drawable somewhere below it; this one enters
    /// every transform it walks, and the sky's `MWRender::CameraRelativeTransform` dereferences the
    /// visitor without checking it, to catch the eye point off a cull. A visitor that is not a cull
    /// visitor takes exactly the branch a null one would have — here and in `osg::AutoTransform`,
    /// the other one that looks — so nothing moves.
    void MirrorTraversal::apply(osg::Transform& node)
    {
        // Nothing an emitter needs is in the chain: a processor reads its world transform off the
        // node path, which `accept` keeps whatever this does.
        if (mStepOnly)
        {
            apply(static_cast<osg::Node&>(node));
            return;
        }

        const osg::Matrix above = mHere;
        node.computeLocalToWorldMatrix(mHere, this);

        apply(static_cast<osg::Node&>(node));

        mHere = above;
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

        mExtractor.addDrawable(
            drawable, identityWith(mPathHash, &drawable), mShading, placed(), mFirstPerson > 0, *mStats);

        mShading.resize(held);
    }

    /// **Everything the content did not hide**, asked of the loader that stamped the bit rather
    /// than named a second time here. `NifOsg::Loader` is what marks a hidden node and a collision
    /// shape, and `Terrain::ObjectPaging` asks it the same question to decide what distant land may
    /// copy — so a host that never configured the loader gets a mask of all ones and walks into
    /// nodes the content said are not there.
    SceneExtractor::SceneExtractor(SceneDesc& scene, Traversals* traversals)
        : mScene(scene)
        , mWalk(std::make_unique<MirrorTraversal>(*this))
        , mTraversals(traversals == nullptr ? mOwnTraversals : *traversals)
        , mTraversalMask(~NifOsg::Loader::getHiddenNodeMask())
    {
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

        mWalk->begin(transform, frame, mTraversals.next(), identitySeed(anchor), stats);
        mWalk->setTraversalMask(mTraversalMask);

        // **Non-const because the walk writes.** It poses every actor it reaches and it runs every
        // state-set controller it finds, which is what makes an actor behind the camera posed and a
        // fire lit; OSG's visitor API is non-const regardless, so the cast happens once, here.
        const_cast<osg::Node&>(node).accept(*mWalk);

        // **Inside the same walk, not beside it.** The chunks a quad tree keeps out of the graph are
        // part of the same frame as everything else — the same epoch, the same stats, the same
        // sweep — and a second `begin` would date them apart from it.
        for (Residency* resident : hidden)
            resident->collect(*mWalk);

        // **After the whole walk, including whatever the residency brought in.** Everything under it
        // has been stepped by now, so what the sprites are read from is a settled world rather than
        // one that depends on where an updater happened to sit among its siblings.
        mEmitters.flush(stats);

        return stats;
    }

    void SceneExtractor::advance()
    {
        mScene.advancePlacement();
    }

    Retirement SceneExtractor::retire()
    {
        Retirement went;

        // **Placements first, because dropping one is what makes its mesh droppable.** A slot the
        // walks no longer reach names geometry nothing is standing on any more, and a sweep that
        // ran the other way round would keep every mesh alive on the strength of a placement it was
        // about to delete.
        //
        // Freed rather than compacted: a slot index is the custom index a hit reads back, so
        // closing the gap would rename every placement above it. The gap is handed to the next
        // thing placed.
        // **Not run at all where every placement was reached**, which is a world that stands still —
        // see `mPlacementsReached`. The sweep below erases nothing then, and it costs a walk of the
        // whole map to say so.
        if (mPlacementsReached < mPlacements.size())
        {
            std::erase_if(mPlacements, [this](const auto& entry) {
                if (entry.second.mEpoch == mEpoch)
                    return false;

                mScene.dropInstance(entry.second.mIndex);
                return true;
            });
        }

        went.mMeshes = mMeshes.retire(mLiveMeshes);
        went.mMaterials = mMaterials.retire(mLiveMaterials);

        // **The emitters are swept beside them and count as neither.** A sprite's texture hangs off
        // no material and an emitter is not in the scene between frames, so nothing else can speak
        // for it — and a frame where no mesh and no material died is exactly the frame where an
        // emitter leaving has to be enough to free what it held.
        mEmitters.retire();

        // **Freed, not compacted, and that is what makes a cell boundary cheap.** Closing the gaps
        // renumbered every mesh and every material, so everything built from an index — which is
        // every bottom-level acceleration structure in the world — had to be built again: nineteen
        // of nineteen crossings on a route across Vvardenfell were full rebuilds. A slot that is
        // freed keeps its index and its room, and the next arrival that fits takes it over. Nothing
        // downstream is told anything, because for it nothing moved.
        mScene.release(mLiveMeshes, mLiveMaterials);

        // **After the sweep and not before it**, so that the walk which fills the next epoch is the
        // one this is measured against. Every entry that survived is still carrying the old stamp
        // and would be dropped on the spot otherwise.
        ++mEpoch;
        mPlacementsReached = 0;

        return went;
    }

    const osg::StateSet* SceneExtractor::animate(osg::Node& node)
    {
        return mMaterials.animate(node, mWalk.get());
    }

    void SceneExtractor::addLight(const SceneUtil::LightSource& source, const osg::NodePath& path,
        const osg::Matrixf& place, double simulationTime, ExtractionStats& stats)
    {
        // **The recorded colours and this frame's scalars, and never the colours the rasterizer
        // draws from.** `lightColour` says why the two are not the same light: a scale of a
        // display-encoded number is not a scale of the light it stands for. It leaves this walk with
        // nothing to know about which of a light's two buffers a frame belongs to.
        //
        // **`LightSource::getEmpty` is not asked**, and that is deliberate: it means the model this
        // light hangs on has no geometry (`CheckEmptyLightVisitor`, `lightutil.cpp:17-38`), which is
        // a rasterizer's reason to skip a light and not a statement that the light is off. A `LIGH`
        // whose mesh is empty still burns.
        const std::optional<Light> made
            = makeLight(lightColour(source, simulationTime), source.getSourceRadius(), place.getTrans());
        if (!made.has_value())
            return;

        mScene.addLight(*made);
        ++stats.mLights;
    }

    void SceneExtractor::addDrawable(const osg::Drawable& drawable, std::size_t who, std::span<const Shading> shading,
        const osg::Matrixf& place, bool firstPerson, ExtractionStats& stats)
    {
        // Asked before the geometry, because a particle system is an `osg::Drawable` with no
        // triangles in it at all: its sprites *are* the drawing, and they leave here as a run of
        // discs rather than as a mesh anything could build a structure over.
        //
        // **Gated on the library before the cast**, which is what every other cast down this walk
        // does and for the reason `apply(osg::Node&)` states: a cell's drawables are `osg`'s and
        // `Terrain`'s, and those answer in a byte where a failed `dynamic_cast` walks the class
        // hierarchy to say the same thing. The two libraries are the ones a system can come from —
        // `osgParticle`'s own, and `NifOsg::ParticleSystem` over it — which is the pair
        // `stepParticles` already names.
        const bool couldEmit = isFrom(drawable, "osgParticle") || isFrom(drawable, "NifOsg");
        if (const auto* particles = couldEmit ? dynamic_cast<const osgParticle::ParticleSystem*>(&drawable) : nullptr)
        {
            mEmitters.add(*particles, shading, place, stats);
            return;
        }

        const MeshResolver::Read read = MeshResolver::readDrawable(drawable);
        if (read.mGeometry == nullptr)
        {
            ++stats.mSkippedUnknown;
            return;
        }

        const osg::Geometry& geometry = *read.mGeometry;

        // Terrain keeps its material on the drawable rather than on the graph, so it is asked first
        // and the state-set walk never sees a chunk.
        const auto* terrain
            = isFrom(geometry, "Terrain") ? dynamic_cast<const Terrain::TerrainDrawable*>(&geometry) : nullptr;
        // **Asked of the drawable and not of the path.** OpenMW marks the water geometry itself, and
        // the node above it is a plain transform shared with anything else hanging there.
        const bool water = isWater(drawable.getNodeMask());

        // **The material before the mesh, because a mesh records the material it arrives wearing.**
        // `MeshRange::mMaterial` says why a static mesh has one to record; a backend bakes its mask
        // against that one, and the two counts past the mesh are what say the loader keeps it so.
        Index material;
        if (water)
            material = mMaterials.resolveWater(stats);
        else if (terrain != nullptr)
            material = mMaterials.resolveTerrain(*terrain, stats);
        else
            material = mMaterials.resolve(shading, stats);

        const Index mesh = mMeshes.resolve(drawable, read, material, stats);
        if (mesh == sNoIndex)
            return;

        // A mesh worn with an animated cutout is one no bake can answer for, and traversal stops for
        // every placement of it; a placement wearing anything but the material its mesh arrived
        // with is the canary — `SceneUtil::CopyOp` shares the state set under every copy, so the
        // only material a mesh can be seen in two of is one a controller made per node.
        const Index arrivedWearing = mScene.getMeshes()[mesh].mMaterial;
        if (arrivedWearing != sNoIndex)
        {
            const Material& worn = mScene.getMaterials()[arrivedWearing];
            if (worn.mAnimated)
                stats.mUnbakeable += worn.isCutout() ? 1 : 0;
            else if (material != arrivedWearing)
                ++stats.mWornOtherwise;
        }

        // **The slot this placement has held since it first appeared**, so a world that stands
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
                .mMaterial = material,
                .mOpacity = fade,
                .mFirstPerson = firstPerson,
            });

            mPlacements.emplace(who, Known{ .mIndex = slot, .mEpoch = mEpoch });
            ++mPlacementsReached;
        }
        else
        {
            // Counted on the way to the stamp rather than by the stamp, so a placement two walks of
            // one epoch both reach is one entry and counts once.
            if (held->second.mEpoch != mEpoch)
                ++mPlacementsReached;

            held->second.mEpoch = mEpoch;
            mScene.moveInstance(held->second.mIndex, place);
            mScene.fadeInstance(held->second.mIndex, fade);
        }

        ++stats.mInstances;
    }

    bool SceneExtractor::isWater(osg::Node::NodeMask mask) const
    {
        return carriesOnly(mask, mWaterMask);
    }

    bool SceneExtractor::isFirstPerson(osg::Node::NodeMask mask) const
    {
        return carriesOnly(mask, mFirstPersonMask);
    }

    bool SceneExtractor::carriesOnly(osg::Node::NodeMask mask, osg::Node::NodeMask named)
    {
        // **Every bit outside the named one, and not merely one inside it.** A node mask is a
        // filter over passes and its default is all ones, so `mask & named` is true for every node
        // that never set one — which in this engine is nearly all of them, and it shaded the whole
        // world as sea. What names the water, or the arms, is that no *other* pass may see it.
        return named != 0 && (mask & ~named) == 0;
    }

}
