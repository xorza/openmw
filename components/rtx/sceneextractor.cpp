#include "sceneextractor.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "error.hpp"
#include "instancerecord.hpp"
#include "lightbuilder.hpp"
#include "nodelibrary.hpp"
#include "shaders/scene.h"
#include "spritelight.hpp"
#include "terraincomposite.hpp"

#include <osg/BlendFunc>
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/NodeVisitor>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/Texture2D>
#include <osg/TriangleIndexFunctor>
#include <osgParticle/Particle>
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>

#include <array>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <functional>
#include <span>

#include <components/nifosg/nifloader.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/surface/material.hpp>
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

        /// Collects triangle indices whatever primitive mode the geometry used.
        ///
        /// Strips, fans and quads all arrive here as triangles, which is the only form an
        /// acceleration structure takes. Degenerate triangles — how a strip restarts — are dropped:
        /// they contribute no surface and a zero-area triangle in a BLAS is wasted traversal.
        struct TriangleCollector
        {
            std::vector<std::uint32_t>* mIndices = nullptr;

            void operator()(unsigned int a, unsigned int b, unsigned int c) const
            {
                if (a == b || b == c || a == c)
                    return;

                mIndices->push_back(a);
                mIndices->push_back(b);
                mIndices->push_back(c);
            }
        };

        /// What the content said this surface is, taken from the nearest ancestor that said it.
        ///
        /// **Nearest wins, which is what a NIF property does.** `NifOsg` stamps a complete material
        /// on the state set it resolves each shape against, so the first one found walking back up
        /// is already the whole answer; an ancestor's is what a shape that carries no state set of
        /// its own inherits.
        const Surface::Material* findDescription(std::span<const Shading> shading)
        {
            for (auto it = shading.rbegin(); it != shading.rend(); ++it)
                if (const Surface::Material* found = Surface::getMaterial(*it->mStateSet))
                    return found;

            return nullptr;
        }

        /// How much of an actor there is under `stateSet`, from the two uniforms the game fades one
        /// with — or `inherited`, where it carries neither.
        ///
        /// **Both off the same state set, which is what tells them from a model's own animation.**
        /// `MWRender::TransparencyUpdater` writes `alpha` and `actorFade` as a pair on a state set
        /// above the whole actor, and the three things that ride them are the distance fade over the
        /// last tenth of `actors processing range`, Invisibility and Chameleon.
        /// `NifOsg::AlphaController` writes `alpha` on its own and writes the same number into the
        /// surface description as well — so a walk that took any `alpha` it met would fade an
        /// animated surface twice.
        ///
        /// The product is what `objects.frag` reaches for the same surface: `diffuseColor.a * alpha
        /// * actorFade`, of which the first factor is already in the material. Nearest wins, exactly
        /// as a rasterizing cull would resolve the uniform, which is what inheriting down the chain
        /// comes to; the scene root carries a pair of ones, so a chain that reaches the bottom
        /// answers the same as no chain.
        float fadeThrough(const osg::StateSet& stateSet, float inherited)
        {
            // Named once for the process. A `std::string` built for every state set of every
            // drawable's chain, every frame, was a measurable share of the walk.
            static const std::string sActorFade("actorFade");
            static const std::string sAlpha("alpha");

            const osg::Uniform* fade = stateSet.getUniform(sActorFade);
            if (fade == nullptr)
                return inherited;

            float actorFade = 1.0f;
            float alpha = 1.0f;
            fade->get(actorFade);
            if (const osg::Uniform* hidden = stateSet.getUniform(sAlpha))
                hidden->get(alpha);

            return actorFade * alpha;
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
        const osg::Texture2D* getTexture(const osg::StateSet& stateSet, unsigned int unit)
        {
            return dynamic_cast<const osg::Texture2D*>(
                stateSet.getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
        }

        /// Counts `image` under its format, and its mips beside it.
        void countFormat(const osg::Image& image, ExtractionStats& stats)
        {
            const ImageFormat format = readFormat(image);

            FormatCount& count = stats.mTextureFormats[static_cast<std::size_t>(format)];
            ++count.mMet;
            if (image.getNumMipmapLevels() > 1)
                ++count.mMipped;

            if (format == ImageFormat::Unnamed)
                stats.mUnnamedFormat = static_cast<std::uint32_t>(image.getPixelFormat());
        }

        /// Every plain counter of `stats`, as one list.
        ///
        /// **The one place they are named, and a binding takes every member or none.** A field
        /// added to `ExtractionStats` and not added here does not compile, which is what keeps the
        /// sum below from dropping one and reporting a number that is short.
        auto countersOf(auto& stats)
        {
            auto& [meshesAdded, materialsAdded, sheets, composites, meshesReused, materialsReused, instances, deformed,
                unskinned, emitters, sprites, skippedUnknown, undescribedMaterials, formats, unnamedFormat,
                skippedEmpty, lights]
                = stats;

            // The two the sum owes something other than addition, and so the two left out of it.
            (void)formats;
            (void)unnamedFormat;

            return std::array{ &meshesAdded, &materialsAdded, &sheets, &composites, &meshesReused, &materialsReused,
                &instances, &deformed, &unskinned, &emitters, &sprites, &skippedUnknown, &undescribedMaterials,
                &skippedEmpty, &lights };
        }

        /// A pass's texture matrix for `unit`, as the `uv * xy + zw` the shader wants.
        ///
        /// OpenSceneGraph hands the matrix to GLSL transposed — it stores rows where GLSL reads
        /// columns — so what a shader multiplies its coordinate by is the transpose of what is here,
        /// and the translation it picks up is this matrix's last row.
        osg::Vec4f getTextureTransform(const osg::StateSet& stateSet, unsigned int unit)
        {
            // Terrain binds two units and no more, so the names are spelled rather than built —
            // and named once for the process, because `getUniform` asks for a `std::string` and a
            // ground material is read for every chunk that arrives.
            static const std::array<std::string, 2> sNames{ "texMat0", "texMat1" };
            assert(unit < sNames.size());

            const osg::Uniform* uniform = stateSet.getUniform(sNames[unit]);
            osg::Matrixf matrix;
            if (uniform == nullptr || !uniform->get(matrix))
                return osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f);

            return osg::Vec4f(matrix(0, 0), matrix(1, 1), matrix(3, 0), matrix(3, 1));
        }

        /// The weights of one blend map, as floats in row order.
        ///
        /// `ESMTerrain` builds these as one byte per texel in `GL_ALPHA`, which is a hundred bytes
        /// for a chunk; widening them costs a few kilobytes a cell and saves requiring 8-bit storage
        /// of the device for the sake of it.
        void readMask(const osg::Image& image, std::vector<float>& weights)
        {
            weights.clear();
            weights.reserve(static_cast<std::size_t>(image.s()) * image.t());
            for (int row = 0; row < image.t(); ++row)
                for (int column = 0; column < image.s(); ++column)
                    weights.push_back(image.getColor(column, row).a());
        }

    }

    ExtractionStats& ExtractionStats::operator+=(const ExtractionStats& other)
    {
        const auto sum = countersOf(*this);
        const auto add = countersOf(other);
        for (std::size_t at = 0; at < sum.size(); ++at)
            *sum[at] += *add[at];

        for (std::size_t at = 0; at < mTextureFormats.size(); ++at)
        {
            mTextureFormats[at].mMet += other.mTextureFormats[at].mMet;
            mTextureFormats[at].mMipped += other.mTextureFormats[at].mMipped;
        }

        if (other.mUnnamedFormat != 0)
            mUnnamedFormat = other.mUnnamedFormat;

        return *this;
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
        flushEmitters(stats);

        return stats;
    }

    void SceneExtractor::advance()
    {
        mScene.advancePlacement();
    }

    namespace
    {
        /// Drops every entry not stamped with `epoch`, and collects what is left.
        ///
        /// The survivors go out unsorted. `SceneDesc::release` takes them that way.
        template <class Map>
        std::uint32_t sweep(Map& known, std::uint64_t epoch, std::vector<Index>& live)
        {
            live.clear();
            live.reserve(known.size());

            std::uint32_t dropped = 0;
            for (auto entry = known.begin(); entry != known.end();)
            {
                if (entry->second.mEpoch == epoch)
                {
                    live.push_back(entry->second.mIndex);
                    ++entry;
                    continue;
                }

                entry = known.erase(entry);
                ++dropped;
            }

            return dropped;
        }
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

        went.mMeshes = sweep(mMeshes, mEpoch, mLiveMeshes);
        went.mMaterials = sweep(mMaterials, mEpoch, mLiveMaterials);

        // The sea's own, which is in no identity map because it is keyed on nothing. It survives a
        // frame that met water and goes with the last cell that had any.
        if (mWaterMaterial != sNoIndex)
        {
            if (mWaterEpoch == mEpoch)
                mLiveMaterials.push_back(mWaterMaterial);
            else
            {
                mWaterMaterial = sNoIndex;
                ++went.mMaterials;
            }
        }

        // The sprite's own references go back with the emitter that took them, which is what makes
        // an emitter leaving enough to free its textures — a frame where no mesh and no material
        // died is exactly the frame the sweep below returns from without looking.
        std::erase_if(mEmitterTextures, [this](const auto& entry) {
            if (entry.second.mEpoch == mEpoch)
                return false;

            mScene.dropTexture(entry.second.mIndex);
            mScene.dropTexture(entry.second.mLighting);

            return true;
        });

        // What `animate` keeps. Swept with everything else because it is keyed on a node the graph
        // can drop, and because a state set held past its node holds the textures in it alive too.
        std::erase_if(mAnimated, [this](const auto& entry) { return entry.second.mEpoch != mEpoch; });

        // **A skin and a set of targets go with the last mesh standing on them**, which the scene
        // decides for itself by counting: what is swept here is only this map's hold on the data.
        // The two agree because a rig is stamped exactly where a mesh on it is met.
        std::erase_if(mRigs, [this](const auto& entry) { return entry.second.mEpoch != mEpoch; });
        std::erase_if(mMorphs, [this](const auto& entry) { return entry.second.mEpoch != mEpoch; });

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

    namespace
    {
        /// The state-set controller on `node`, from whichever callback chain carries it.
        ///
        /// **Both chains, because `NifOsg` picks between them by a flag on the content.** Anything
        /// marked `AnimFlag_AutoPlay` is hung from a cull callback and everything else from an
        /// update callback; what they animate — a flipbook, a scrolling UV, an alpha, a material
        /// colour — is the same either way.
        SceneUtil::StateSetUpdater* findUpdater(osg::Node& node)
        {
            for (osg::Callback* chain : { node.getCullCallback(), node.getUpdateCallback() })
                for (osg::Callback* callback = chain; callback != nullptr; callback = callback->getNestedCallback())
                    if (auto* updater = dynamic_cast<SceneUtil::StateSetUpdater*>(callback))
                        return updater;

            return nullptr;
        }
    }

    const osg::StateSet* SceneExtractor::animate(osg::Node& node)
    {
        // Asked of every node in the graph every frame, and nearly all of a cell hangs off no
        // callback at all.
        if (node.getCullCallback() == nullptr && node.getUpdateCallback() == nullptr)
            return nullptr;

        SceneUtil::StateSetUpdater* updater = findUpdater(node);
        if (updater == nullptr)
            return nullptr;

        auto [entry, arrived] = mAnimated.try_emplace(&node);
        if (arrived)
        {
            // **A copy of what the node already wears, and a shallow one.** `applyCull` starts from
            // an empty state set and lets the rasterizer's state stack supply everything it does
            // not itself write — which a mirror reading one state set per surface cannot do, so a
            // fire would lose its material along with its animation. Shallow because an updater
            // that means to write an attribute makes itself a private copy in `setDefaults`, which
            // is the contract `applyUpdate` already rests on.
            //
            // The node's own is read rather than created: `getOrCreateStateSet` would leave an
            // empty one behind on a node that had none, and the walk above would then push it over
            // the material a parent was contributing.
            const osg::StateSet* base = node.getStateSet();
            entry->second.mStateSet
                = base != nullptr ? new osg::StateSet(*base, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet;
            updater->setDefaults(entry->second.mStateSet);
        }

        entry->second.mEpoch = mEpoch;
        updater->apply(entry->second.mStateSet, mWalk.get());
        return entry->second.mStateSet;
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

    /// Nearly everything in a cell is an `osg::Geometry` and answers in one virtual call. A skinned
    /// body and a morphed face are not: each is an `osg::Drawable` over a source geometry, and the
    /// source is what this reads — the bind pose a skin is computed from, the base a morph starts
    /// from. **Not the double-buffered copy a cull traversal writes**, which no walk of this
    /// renderer runs any more: the pose is bone rows and weights handed to the device, and the
    /// device computes the vertices where the structure is refitted from them.
    ///
    /// **A rig no update traversal has resolved is read as it stands.** Its bones are what
    /// `RigGeometry::updateBounds` finds under the update traversal, and a rig with none has nothing
    /// to be posed against; the rasterizer draws that rig in its bind pose, and so does this. A morph
    /// with no target past its base has nothing to move either, and is a static mesh whose
    /// positions are the base.
    SceneExtractor::Read SceneExtractor::readDrawable(const osg::Drawable& drawable)
    {
        if (const osg::Geometry* geometry = drawable.asGeometry())
            return Read{ .mGeometry = geometry };

        if (const auto* rig = dynamic_cast<const SceneUtil::RigGeometry*>(&drawable))
        {
            const bool skinned = rig->getInfluenceData() != nullptr && !rig->getBones().empty();
            return Read{ .mGeometry = rig->getSourceGeometry().get(),
                .mDeform = skinned ? Deform::Rig : Deform::None,
                .mRig = rig };
        }

        if (const auto* morph = dynamic_cast<const SceneUtil::MorphGeometry*>(&drawable))
        {
            const bool moving = morph->getMorphTargetList().size() > 1;
            return Read{ .mGeometry = morph->getSourceGeometry().get(),
                .mDeform = moving ? Deform::Morph : Deform::None,
                .mMorph = morph };
        }

        return Read{};
    }

    namespace
    {
        /// A geometry's per-vertex positions and normals.
        struct VertexArrays
        {
            std::span<const osg::Vec3f> mPositions;

            /// Empty only where the geometry names no normal at all. A per-vertex array is taken as
            /// it stands and a single overall one is spread across the vertices, which is the same
            /// answer at every point of a flat surface.
            std::span<const osg::Vec3f> mNormals;
        };

        /// @param flat scratch for an overall normal spread across the vertices. Refilled here and
        ///        borrowed by the returned span, so it has to outlive the read.
        VertexArrays readVertices(const osg::Geometry& geometry, std::vector<osg::Vec3f>& flat)
        {
            VertexArrays arrays;

            const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (positions == nullptr)
                return arrays;

            arrays.mPositions = std::span(positions->asVector());

            const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray());
            if (normals == nullptr || normals->empty())
                return arrays;

            if (normals->size() == positions->size())
            {
                arrays.mNormals = std::span(normals->asVector());
                return arrays;
            }

            // **One normal for the whole drawable is a normal, and dropping it made the sea flat
            // black.** `SceneUtil::createWaterGeometry` binds exactly this — a thousand vertices and
            // one `(0, 0, 1)` — so the game's water mirrored with no normal at all, and shading a
            // surface by a zero vector produces radiance that the frame's own exposure then reads.
            // Everything else in the picture goes with it.
            if (normals->getBinding() != osg::Array::BIND_OVERALL)
                return arrays;

            flat.assign(positions->size(), normals->at(0));
            arrays.mNormals = std::span(flat);
            return arrays;
        }

        /// How many vertices a geometry has, or nought where it holds none it can be read for.
        /// Asked on its own where the count is the whole question, so a body met again does not
        /// spread its normals to find out.
        std::size_t vertexCountOf(const osg::Geometry& geometry)
        {
            const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            return positions != nullptr ? positions->size() : 0;
        }
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
            addEmitter(*particles, shading, place, stats);
            return;
        }

        const Read read = readDrawable(drawable);
        if (read.mGeometry == nullptr)
        {
            ++stats.mSkippedUnknown;
            return;
        }

        const osg::Geometry& geometry = *read.mGeometry;
        const Index mesh = resolveMesh(drawable, read, stats);
        if (mesh == sNoIndex)
            return;

        // Terrain keeps its material on the drawable rather than on the graph, so it is asked first
        // and the state-set walk never sees a chunk.
        const auto* terrain
            = isFrom(geometry, "Terrain") ? dynamic_cast<const Terrain::TerrainDrawable*>(&geometry) : nullptr;
        // **Asked of the drawable and not of the path.** OpenMW marks the water geometry itself, and
        // the node above it is a plain transform shared with anything else hanging there.
        const bool water = isWater(drawable.getNodeMask());

        Index material;
        if (water)
            material = resolveWaterMaterial(stats);
        else if (terrain != nullptr)
            material = resolveTerrainMaterial(*terrain, stats);
        else
            material = resolveMaterial(shading, stats);

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

    namespace
    {
        /// Whether the nearest pass on the path adds to the frame rather than covering it.
        ///
        /// **The nearest state set that has a blend function, not simply the nearest one.** A
        /// particle system carries a state set of its own that sets neither blending nor texture —
        /// `NifOsg` puts both on the transform above it — so asking the drawable's own would answer
        /// "covers" for every flame in the game.
        ///
        /// The split is the whole of what tells a flame from a puff of smoke, and 474 of the game's
        /// 678 emitters are on the adding side. One that blends over is an albedo and has to be lit;
        /// one that adds *is* light and must not be.
        bool addsLight(std::span<const Shading> shading)
        {
            for (auto it = shading.rbegin(); it != shading.rend(); ++it)
            {
                const auto* blend
                    = dynamic_cast<const osg::BlendFunc*>(it->mStateSet->getAttribute(osg::StateAttribute::BLENDFUNC));
                if (blend == nullptr)
                    continue;

                return blend->getSource() == osg::BlendFunc::SRC_ALPHA
                    && blend->getDestination() == osg::BlendFunc::ONE;
            }

            return false;
        }

        /// The uniform scale a placement carries, as the length of its first basis row.
        ///
        /// A sprite's size is in the particle system's own coordinates — `LOCAL_COORDINATES` is what
        /// `NifOsg` sets — so the quad the rasterizer would draw is scaled by the modelview along
        /// with everything else. Morrowind scales references uniformly, so one number says it.
        float scaleOf(const osg::Matrixf& place)
        {
            return osg::Vec3f(place(0, 0), place(0, 1), place(0, 2)).length();
        }
    }

    void SceneExtractor::addEmitter(const osgParticle::ParticleSystem& particles, std::span<const Shading> shading,
        const osg::Matrixf& place, ExtractionStats& stats)
    {
        // A particle's whole silhouette is its texture's alpha, so an emitter with no texture has
        // nothing to draw — not a white disc, which is what sampling nothing would give it.
        const Surface::Material* described = findDescription(shading);
        if (described == nullptr)
        {
            ++stats.mUndescribedMaterials;
            return;
        }

        const osg::Image* sprite = described->getTexture(Surface::TextureRole::Diffuse);
        if (sprite == nullptr || sprite->getFileName().empty())
            return;

        // **Registered the first time the emitter is seen, and not the first time it has a
        // particle alive.** The texture array is uploaded when the scene is built; a flame that was
        // empty at load and lights up two hundred frames later would otherwise add a texture on a
        // frame that only re-places what is already there, and index past the array it is sampling.
        //
        // Kept in a map of its own because nothing else can speak for it when the scene is swept: a
        // sprite's texture is on no material, and an emitter is not in the scene between frames.
        auto [known, arrived] = mEmitterTextures.try_emplace(&particles);
        if (arrived)
        {
            const VFS::Path::Normalized path(sprite->getFileName());
            known->second.mIndex = mScene.addTexture(path);

            // The bake is keyed on the file, so two emitters drawing with one texture share one
            // bake, and it is made when the texture is opened for upload — `SceneTextures`.
            known->second.mLighting = mScene.addBakedTexture(SpriteLightMap::keyFor(path));

            // **Held, because nothing else can name them.** An emitter is a placement and is thrown
            // away every frame, so this entry is the only lasting thing that says the sprite is in
            // use; the scene frees the slots when the sweep below lets go of them.
            mScene.holdTexture(known->second.mIndex);
            mScene.holdTexture(known->second.mLighting);
        }

        known->second.mEpoch = mEpoch;

        // **Noted now and read when the walk is over.** Whether this system has been integrated
        // this frame depends on where its `ParticleSystemUpdater` sits among its siblings — above
        // it in everything `NifOsg` builds, but that is the content's promise and not this walk's.
        // Reading after the walk has settled is what makes the question stop existing.
        mPending.push_back(PendingEmitter{
            .mParticles = &particles,
            .mPlace = place,
            .mTexture = known->second.mIndex,
            .mLighting = known->second.mLighting,

            .mLight = addsLight(shading),
            .mSprite = sprite,

        });
    }

    void SceneExtractor::flushEmitters(ExtractionStats& stats)
    {
        for (const PendingEmitter& pending : mPending)
            placeSprites(pending, stats);

        mPending.clear();
    }

    void SceneExtractor::placeSprites(const PendingEmitter& pending, ExtractionStats& stats)
    {
        const osgParticle::ParticleSystem& particles = *pending.mParticles;
        const osg::Matrixf& place = pending.mPlace;

        const float scale = scaleOf(place);

        mSpriteScratch.clear();
        const int held = particles.numParticles();
        for (int at = 0; at < held; ++at)
        {
            const osgParticle::Particle* particle = particles.getParticle(at);

            // A dead slot keeps its last position and is waiting to be born again. Drawing one is a
            // spark frozen where the previous one expired.
            if (!particle->isAlive())
                continue;

            const float radius = particle->getCurrentSize() * scale;
            if (!(radius > 0.0f))
                continue;

            // `getCurrentColor`'s alpha and `getCurrentAlpha` are two separate ramps and the
            // rasterizer multiplies them. OpenMW's `ParticleColorAffector` writes the record's
            // colour ramp into the first with its alpha forced to one and the alpha into the
            // second, so in this content the product is the second — and multiplying both is what
            // keeps that a fact about the data rather than an assumption in the reader.
            const osg::Vec4f colour = particle->getCurrentColor();
            const float alpha = colour.a() * particle->getCurrentAlpha();
            if (!(alpha > 0.0f))
                continue;

            // **Both ends through the same matrix, and the difference taken here.** `place` is where
            // the emitter stands *now*; a system carried by an actor moved between the two frames and
            // this does not know by how much, so what comes out is the particle's own travel and not
            // its travel plus its emitter's. For rain, snow and ash — which are placed in the world
            // and not on anybody — the two are the same thing, and those are the populations that
            // cross a frame fast enough for the difference to be the picture.
            const osg::Vec3f stood = particle->getPosition() * place;
            const osg::Vec3f came = particle->getPreviousPosition() * place;

            mSpriteScratch.push_back(Sprite{
                .mPosition = stood,
                .mRadius = radius,
                .mColour = osg::Vec3f(colour.r(), colour.g(), colour.b()),
                .mAlpha = alpha,
                .mMoved = stood - came,
            });
        }

        if (mSpriteScratch.empty())
            return;

        countFormat(*pending.mSprite, stats);

        // **Which way the quad faces, and `osgParticle` offers two answers.** A `BILLBOARD` system's
        // axes are the screen's and are recomputed into view space every frame, which is a disc
        // facing the eye and needs nothing carried across. A `FIXED` one's are used exactly as they
        // were authored, so its quad hangs in the world at an orientation of its own — and that is
        // the mode Morrowind's rain is built on: an X axis squashed to a tenth against a Y pointing
        // straight down is a falling streak rather than a round drop.
        //
        // Rotated into the world by the placement and not normalised, because their length is the
        // shape rather than a direction.
        osg::Vec3f across;
        osg::Vec3f upward;
        if (particles.getParticleAlignment() == osgParticle::ParticleSystem::FIXED)
        {
            across = osg::Matrixf::transform3x3(particles.getAlignVectorX(), place);
            upward = osg::Matrixf::transform3x3(particles.getAlignVectorY(), place);
        }

        mScene.addEmitter(mSpriteScratch, pending.mTexture, pending.mLight, across, upward, pending.mLighting);

        ++stats.mEmitters;
        stats.mSprites += static_cast<std::uint32_t>(mSpriteScratch.size());
    }

    Index SceneExtractor::resolveTerrainMaterial(const Terrain::TerrainDrawable& terrain, ExtractionStats& stats)
    {
        const Terrain::TerrainDrawable::PassVector& passes = terrain.getPasses();
        if (passes.empty())
            return sNoIndex;

        // The first pass is as good an identity as the chunk itself and is already a state set, so
        // terrain shares the material map with everything else.
        const osg::StateSet* identity = passes.front().get();
        const auto known = mMaterials.find(identity);
        if (known != mMaterials.end())
        {
            ++stats.mMaterialsReused;
            known->second.mEpoch = mEpoch;
            return known->second.mIndex;
        }

        Material material;
        material.mKind = MaterialKind::Terrain;

        mLayerScratch.clear();

        for (const osg::ref_ptr<osg::StateSet>& pass : passes)
        {
            const Surface::Material* described = Surface::getMaterial(*pass);
            if (described == nullptr)
            {
                ++stats.mUndescribedMaterials;
                continue;
            }

            MaterialLayer layer;
            layer.mDiffuse = takeTexture(described->getTexture(Surface::TextureRole::Diffuse), stats);
            if (layer.mDiffuse == sNoIndex)
                continue;

            layer.mDiffuseTransform = getTextureTransform(*pass, 0);

            // A chunk of a single ground type is given no blend map at all, and stays at full weight.
            const osg::Texture2D* mask = getTexture(*pass, 1);
            if (mask != nullptr && mask->getImage(0) != nullptr)
            {
                const osg::Image& image = *mask->getImage(0);
                readMask(image, mMaskScratch);

                // The two sides are what `SceneDesc::release` reconstructs the run's length from,
                // so a mask that is not as long as its own grid leaks the difference.
                assert(mMaskScratch.size() == static_cast<std::size_t>(image.s()) * image.t());

                layer.mMaskOffset = mScene.addMask(mMaskScratch);
                layer.mMaskWidth = static_cast<std::uint16_t>(image.s());
                layer.mMaskHeight = static_cast<std::uint16_t>(image.t());
                layer.mMaskTransform = getTextureTransform(*pass, 1);
            }

            mLayerScratch.push_back(layer);
        }

        if (mLayerScratch.empty())
            return sNoIndex;

        const Span run = mScene.addLayers(mLayerScratch);
        material.mLayerOffset = run.mOffset;
        material.mLayerCount = run.mCount;

        // **A chunk this wide is a shading question and not only a texturing one.** It covers whole
        // cells and carries every ground type in them, so shading it live costs a mask lookup and a
        // texture fetch per layer at every hit — and once there is distance to look at, distant hits
        // are most of the pixels. Past a cell the stack is flattened into one texture and a hit
        // takes a single fetch; the layers stay, because they are the recipe the bake reads.
        //
        // **A single layer is already a single fetch**, and flattening one would do nothing but
        // resample a tiling ground texture into something coarser than the file it came from.
        const osg::BoundingBox& bounds = terrain.getBoundingBox();
        const float across = std::max(bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin());

        if (mLayerScratch.size() > 1 && across >= sCompositeFrom)
        {
            material.mFlatten = true;
            ++stats.mComposites;
        }

        const Index index = mScene.addMaterial(material);
        mMaterials.emplace(identity, Known{ .mIndex = index, .mEpoch = mEpoch });
        ++stats.mMaterialsAdded;
        return index;
    }

    namespace
    {
        /// The box a drawable's own bound reaches, in its own space.
        ///
        /// **The sphere and not the box, because that is the one a rig keeps.** `RigGeometry::
        /// updateBounds` writes its sphere straight into the drawable and marks it computed, and
        /// leaves the box to be recomputed from a callback that answers nothing — so asking for the
        /// box would overwrite what the update worked out from the bone spheres. A sphere is a
        /// looser box than the vertices would give, and it is the game's own number: the pose is on
        /// the device and there are no vertices here to walk.
        osg::BoundingBoxf reachOf(const osg::Drawable& drawable)
        {
            const osg::BoundingSphere& sphere = drawable.getBound();
            if (!sphere.valid())
                return osg::BoundingBoxf();

            const osg::Vec3f centre(sphere.center());
            const osg::Vec3f reach(sphere.radius(), sphere.radius(), sphere.radius());
            return osg::BoundingBoxf(centre - reach, centre + reach);
        }

        /// A morph's base target, which `MorphGeometry::cull` reads its positions from. The source
        /// geometry's own array is what `NifOsg` built the drawable from and the two agree in every
        /// file it builds, so the length is asserted and the base is what is read.
        std::span<const osg::Vec3f> baseOf(const SceneUtil::MorphGeometry& morph)
        {
            const osg::Vec3Array* base = morph.getMorphTarget(0).getOffsets();
            assert(base != nullptr && "a morph whose base is no array");
            return std::span(base->asVector());
        }
    }

    Index SceneExtractor::resolveMesh(const osg::Drawable& drawable, const Read& read, ExtractionStats& stats)
    {
        const osg::Geometry& geometry = *read.mGeometry;

        if (const auto known = mMeshes.find(&drawable); known != mMeshes.end())
        {
            const Index mesh = known->second.mIndex;
            const MeshRange& range = mScene.getMeshes()[mesh];

            // Nothing else in the map is re-read: the whole point of it is that a crate met again is
            // the crate already uploaded, and a cell is tens of thousands of these a frame.
            if (read.mDeform == Deform::None && range.mDeform == Deform::None)
            {
                ++stats.mMeshesReused;
                known->second.mEpoch = mEpoch;
                return mesh;
            }

            // **What says the slot still fits, and it has to be asked.** The drawable is the same
            // object — the map owns its key, so it cannot be a different one wearing the same
            // address — but a deforming drawable is a shell over a source geometry the engine may
            // replace, and a rig re-pointed at a longer mesh is the same rig. Posing that into the
            // old slot is not a wrong pose: the slot is a run inside one shared vertex buffer, and
            // the kernel would write past it over the meshes that follow.
            //
            // Where the source, the kind or the skin differs the entry is wrong rather than stale,
            // so it goes and the geometry is mirrored afresh. The slot it abandons keeps the epoch
            // it had and the next sweep takes it.
            const std::size_t vertices
                = read.mDeform == Deform::Morph ? baseOf(*read.mMorph).size() : vertexCountOf(geometry);

            // What the drawable's skin or targets resolve to, where the mirror has met them, and
            // `sNoIndex` where it has not or where the drawable stands — which is what a slot that
            // stands holds too. A morph whose targets changed count under the same base is another
            // morph, so the count is asked beside the identity.
            Index deformer = sNoIndex;
            if (read.mDeform == Deform::Rig)
            {
                if (const auto rig = mRigs.find(read.mRig->getInfluenceData()); rig != mRigs.end())
                    deformer = rig->second.mIndex;
            }
            else if (read.mDeform == Deform::Morph)
            {
                const auto morph = mMorphs.find(read.mMorph->getMorphTarget(0).getOffsets());
                if (morph != mMorphs.end()
                    && mScene.getMorphs()[morph->second.mIndex].mTargetCount
                        == read.mMorph->getMorphTargetList().size())
                    deformer = morph->second.mIndex;
            }

            if (vertices == range.mVertexCount && read.mDeform == range.mDeform && deformer == range.mDeformer)
            {
                ++stats.mMeshesReused;
                known->second.mEpoch = mEpoch;

                // A pose is rows and not vertices, which is why the mirror pays a few dozen
                // matrices for what is actually moving. The skin is stamped with the mesh, which is
                // what keeps the sweep's two answers one answer.
                if (read.mDeform == Deform::Rig)
                {
                    mRigs.find(read.mRig->getInfluenceData())->second.mEpoch = mEpoch;
                    poseRig(mesh, *read.mRig);
                    ++stats.mDeformed;
                }
                else if (read.mDeform == Deform::Morph)
                {
                    mMorphs.find(read.mMorph->getMorphTarget(0).getOffsets())->second.mEpoch = mEpoch;
                    poseMorph(mesh, *read.mMorph);
                    ++stats.mDeformed;
                }

                return mesh;
            }

            mMeshes.erase(known);
        }

        VertexArrays arrays = readVertices(geometry, mFlatNormalScratch);

        // A morph starts from its base target and not from the source's array, because that is
        // what `MorphGeometry::cull` starts from. The normals and everything else are the source's.
        if (read.mDeform == Deform::Morph)
        {
            const std::span<const osg::Vec3f> base = baseOf(*read.mMorph);
            if (base.size() != arrays.mPositions.size())
                throw Error("a morphed face of " + std::to_string(arrays.mPositions.size())
                    + " vertices whose base target has " + std::to_string(base.size()));

            arrays.mPositions = base;
        }

        if (arrays.mPositions.empty())
        {
            ++stats.mSkippedEmpty;
            return sNoIndex;
        }

        if (read.mRig != nullptr && read.mDeform == Deform::None)
            ++stats.mUnskinned;

        mIndexScratch.clear();
        osg::TriangleIndexFunctor<TriangleCollector> collector;
        collector.mIndices = &mIndexScratch;
        geometry.accept(collector);

        if (mIndexScratch.empty())
        {
            ++stats.mSkippedEmpty;
            return sNoIndex;
        }

        std::span<const osg::Vec2f> texCoords;
        const auto* texCoordArray = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
        if (texCoordArray != nullptr && texCoordArray->size() == arrays.mPositions.size())
            texCoords = std::span(texCoordArray->asVector());

        // Before the mesh is written, so the copy the content drew for a card's back never reaches
        // a structure. Once per drawable and never for a pose: a rig moves the two copies together,
        // so the pairs found in the bind pose are the pairs.
        const FoldedShape shape = mShapeFold.fold(arrays.mPositions, mIndexScratch);
        if (shape.mSheet)
            ++stats.mSheets;

        // What poses it, added once per skin and once per set of targets however many drawables
        // share them, and stamped here so the sweep keeps it for as long as a mesh stands on it.
        Index deformer = sNoIndex;
        if (read.mDeform == Deform::Rig)
            deformer = resolveRig(*read.mRig);
        else if (read.mDeform == Deform::Morph)
            deformer = resolveMorph(*read.mMorph);

        if (deformer != sNoIndex)
        {
            const Index skinned = read.mDeform == Deform::Rig ? mScene.getRigs()[deformer].mVertexCount
                                                              : mScene.getMorphs()[deformer].mVertexCount;
            if (skinned != arrays.mPositions.size())
                throw Error("a deforming mesh of " + std::to_string(arrays.mPositions.size())
                    + " vertices on a rig or morph of " + std::to_string(skinned));
        }

        const Index mesh = mScene.addMesh(
            arrays.mPositions, arrays.mNormals, texCoords, mIndexScratch, shape, read.mDeform, deformer);
        mMeshes.emplace(&drawable, Known{ .mIndex = mesh, .mEpoch = mEpoch });
        ++stats.mMeshesAdded;

        // Posed on arrival as on every frame after: the bind pose the mesh holds is what a pose is
        // computed from, and never what is traced.
        if (read.mDeform == Deform::Rig)
        {
            poseRig(mesh, *read.mRig);
            ++stats.mDeformed;
        }
        else if (read.mDeform == Deform::Morph)
        {
            poseMorph(mesh, *read.mMorph);
            ++stats.mDeformed;
        }

        return mesh;
    }

    Index SceneExtractor::resolveRig(const SceneUtil::RigGeometry& rig)
    {
        const SceneUtil::RigGeometry::InfluenceData* skin = rig.getInfluenceData();
        assert(skin != nullptr);

        const std::size_t vertices = vertexCountOf(*rig.getSourceGeometry());

        // **A skin rewritten in place under the same address is a new skin.** `setInfluences` on a
        // rig the mirror has met writes into the `InfluenceData` every copy shares, so what the map
        // holds describes a mesh of another length; the rig it named stays for the meshes still on
        // it and goes with the last of them, and this drawable gets one of its own.
        auto [known, arrived] = mRigs.try_emplace(skin);
        known->second.mEpoch = mEpoch;
        if (!arrived && mScene.getRigs()[known->second.mIndex].mVertexCount == vertices)
            return known->second.mIndex;

        // **The groups flattened into a run per vertex.** `RigGeometry::setInfluences` gathers the
        // vertices that share one weight list so the rasterizer blends each list once; a kernel
        // blends per lane and wants to find its list from its vertex, which is what the run word
        // is. A vertex in no group is a run of nothing, as the rasterizer leaves it at the origin.

        mRunScratch.assign(vertices, 0);
        mInfluenceScratch.clear();
        for (const auto& [weights, group] : skin->mInfluences)
        {
            if (weights.size() > Shaders::RUN_COUNT_MASK)
                throw Error("a vertex skinned by " + std::to_string(weights.size()) + " bones, past the "
                    + std::to_string(Shaders::RUN_COUNT_MASK) + " a run word holds");

            const auto first = static_cast<std::uint32_t>(mInfluenceScratch.size());
            for (const auto& [bone, weight] : weights)
                mInfluenceScratch.push_back(Shaders::GpuInfluence{
                    .mBone = static_cast<std::uint32_t>(bone),
                    .mWeight = weight,
                });

            const std::uint32_t run = (first << Shaders::RUN_COUNT_BITS) | static_cast<std::uint32_t>(weights.size());
            for (const unsigned short vertex : group)
            {
                if (vertex >= vertices)
                    throw Error("a skin naming vertex " + std::to_string(vertex) + " of a mesh with "
                        + std::to_string(vertices));

                mRunScratch[vertex] = run;
            }
        }

        known->second.mIndex = mScene.addRig(mRunScratch, mInfluenceScratch, static_cast<Index>(skin->mBones.size()));
        return known->second.mIndex;
    }

    Index SceneExtractor::resolveMorph(const SceneUtil::MorphGeometry& morph)
    {
        const SceneUtil::MorphGeometry::MorphTargetList& targets = morph.getMorphTargetList();
        assert(targets.size() > 1);

        const std::size_t vertices = baseOf(morph).size();

        // A set of targets grown or shrunk under the same base is a new set, for the reason a
        // rewritten skin is a new skin.
        auto [known, arrived] = mMorphs.try_emplace(targets[0].getOffsets());
        known->second.mEpoch = mEpoch;
        if (!arrived)
        {
            const Morph& held = mScene.getMorphs()[known->second.mIndex];
            if (held.mTargetCount == targets.size() && held.mVertexCount == vertices)
                return known->second.mIndex;
        }

        // Every target's offsets laid end to end, the base's included as a run of zeroes so the
        // table's target `k` is the drawable's target `k` and a weight indexes both the same way.
        // `MorphGeometry::cull` reads target `k` as `offsets[k][vertex]` for every `k` past the
        // base; a target shorter than the base is read as far as it goes and the rest left alone,
        // which a zero past its end is.
        mOffsetScratch.assign(vertices * targets.size(), osg::Vec3f());
        for (std::size_t target = 1; target < targets.size(); ++target)
        {
            const osg::Vec3Array* offsets = targets[target].getOffsets();
            if (offsets == nullptr)
                continue;

            const std::size_t count = std::min<std::size_t>(offsets->size(), vertices);
            std::copy_n(offsets->begin(), count, mOffsetScratch.begin() + target * vertices);
        }

        known->second.mIndex = mScene.addMorph(mOffsetScratch, static_cast<Index>(targets.size()));
        return known->second.mIndex;
    }

    void SceneExtractor::poseRig(Index mesh, const SceneUtil::RigGeometry& rig)
    {
        const SceneUtil::RigGeometry::InfluenceData& skin = *rig.getInfluenceData();
        const std::span<SceneUtil::Bone* const> bones = rig.getBones();
        assert(bones.size() == skin.mBones.size());

        // `RigGeometry::cull`'s arithmetic, row for row: each bone's inverse bind by its
        // skeleton-space matrix, and the skin's transform after the blend — composed into every
        // bone here, which is the same product because the blend is linear and the transform is
        // affine. A bone the skeleton has not got contributes nothing, as it does there.
        //
        // **From the matrices the update traversal left.** `RigGeometry::updateBounds` runs
        // `Skeleton::updateBoneMatrices` under it for every active skeleton and on the first frame
        // regardless, and a skeleton it skipped is one whose bones did not move — so what is here
        // is this frame's pose or the last one, and either is what the rasterizer would show.
        osg::Matrixf transform = skin.mTransform;
        if (const osg::RefMatrix* skinToSkel = rig.getSkinToSkelMatrix())
            transform = (*skinToSkel) * skin.mTransform;

        mBoneScratch.clear();
        mBoneScratch.reserve(bones.size());
        for (std::size_t at = 0; at < bones.size(); ++at)
        {
            if (bones[at] == nullptr)
            {
                mBoneScratch.push_back(Shaders::GpuBone{});
                continue;
            }

            mBoneScratch.push_back(
                toGpuBone(skin.mBones[at].mInvBindMatrix * bones[at]->mMatrixInSkeletonSpace * transform));
        }

        mScene.poseRig(mesh, mBoneScratch, reachOf(rig));
    }

    void SceneExtractor::poseMorph(Index mesh, const SceneUtil::MorphGeometry& morph)
    {
        const SceneUtil::MorphGeometry::MorphTargetList& targets = morph.getMorphTargetList();

        mWeightScratch.clear();
        mWeightScratch.reserve(targets.size());
        for (const SceneUtil::MorphGeometry::MorphTarget& target : targets)
            mWeightScratch.push_back(target.getWeight());

        mScene.poseMorph(mesh, mWeightScratch, reachOf(morph));
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

    Index SceneExtractor::resolveWaterMaterial(ExtractionStats& stats)
    {
        // **One material for the sea, and it is keyed on nothing.** Water has no albedo — what it
        // looks like is what is behind and above it, worked out from the world position — so there
        // is nothing on a state set worth reading, and reading one is actively wrong twice over.
        //
        // `MWRender::Water` animates its surface with a `SceneUtil::StateSetUpdater`, which swaps
        // the node's state set between two copies of its own every frame: keyed on the address, the
        // mirror saw a new material each frame and swept the one before it, for a surface that had
        // not changed. And with `water shader = true` there is no state set on the node at all,
        // because that one is pushed from a cull callback the mirror runs outside of.
        if (mWaterMaterial != sNoIndex)
        {
            ++stats.mMaterialsReused;
            mWaterEpoch = mEpoch;
            return mWaterMaterial;
        }

        mWaterMaterial = mScene.addMaterial(Material{ .mKind = MaterialKind::Water });
        mWaterEpoch = mEpoch;
        ++stats.mMaterialsAdded;
        return mWaterMaterial;
    }

    Index SceneExtractor::resolveMaterial(std::span<const Shading> shading, ExtractionStats& stats)
    {
        if (shading.empty())
            return sNoIndex;

        // The material's identity is the state set nearest the drawable. Two drawables that share
        // it share their shading: OpenMW's optimizer collapses equivalent state sets into one
        // object, so sharing the pointer means sharing the values, and what the parents above
        // contribute in this graph is light and render-bin state rather than material.
        const Shading& own = shading.back();

        const auto known = mMaterials.find(own.mStateSet);
        if (known != mMaterials.end())
        {
            ++stats.mMaterialsReused;
            known->second.mEpoch = mEpoch;

            // **Read again, because a controller rewrote it since the last frame.** The state set
            // is the same object — that is what lets the material keep its slot and every placement
            // standing on it stay where it is — and everything inside it is this frame's.
            if (own.mAnimated)
                mScene.setMaterial(known->second.mIndex, readMaterial(shading, stats));

            return known->second.mIndex;
        }

        const Index index = mScene.addMaterial(readMaterial(shading, stats));
        mMaterials.emplace(own.mStateSet, Known{ .mIndex = index, .mEpoch = mEpoch });
        ++stats.mMaterialsAdded;
        return index;
    }

    Index SceneExtractor::takeTexture(const osg::Image* image, ExtractionStats& stats)
    {
        if (image == nullptr || image->getFileName().empty())
            return sNoIndex;

        countFormat(*image, stats);
        return mScene.addTexture(VFS::Path::Normalized(image->getFileName()));
    }

    Material SceneExtractor::readMaterial(std::span<const Shading> shading, ExtractionStats& stats)
    {
        Material material;

        const Surface::Material* described = findDescription(shading);
        if (described == nullptr)
        {
            ++stats.mUndescribedMaterials;
            return material;
        }

        material.mDiffuse = takeTexture(described->getTexture(Surface::TextureRole::Diffuse), stats);
        material.mEmissive = takeTexture(described->getTexture(Surface::TextureRole::Emissive), stats);

        // The two normal roles differ in what the alpha channel holds, and parallax is a rasterizer
        // feature this renderer does not have: to a ray tracer they are the same texture.
        material.mNormal = takeTexture(described->getTexture(Surface::TextureRole::Normal), stats);
        if (material.mNormal == sNoIndex)
            material.mNormal = takeTexture(described->getTexture(Surface::TextureRole::NormalHeight), stats);

        material.mAlphaRef = described->mAlphaRef;
        switch (described->mAlphaMode)
        {
            case Surface::AlphaMode::Blend:
                material.mAlphaMode = AlphaMode::Blend;
                break;
            case Surface::AlphaMode::Cutout:
                material.mAlphaMode = AlphaMode::Cutout;
                break;
            case Surface::AlphaMode::Opaque:
                break;
        }

        material.mTwoSided = described->mTwoSided;
        material.mDiffuseColour = described->mDiffuseColour;

        // Folded together because the game's own shader only ever uses their product.
        material.mEmissiveColour = described->mEmissiveColour * described->mEmissiveMult;

        // **Scaled about the middle of the texture, then offset**, which is what `NifOsg` builds its
        // texture matrix from — so `(uv - 0.5) * scale + 0.5 + offset`, resolved here into the
        // `uv * xy + zw` the sampler takes. Doing the arithmetic once on the host keeps two
        // multiplies and an add out of every texture fetch in the frame.
        const osg::Vec2f scale = described->mTextureScale;
        const osg::Vec2f offset = described->mTextureOffset;
        material.mTextureTransform = osg::Vec4f(
            scale.x(), scale.y(), 0.5f * (1.0f - scale.x()) + offset.x(), 0.5f * (1.0f - scale.y()) + offset.y());

        return material;
    }
}
