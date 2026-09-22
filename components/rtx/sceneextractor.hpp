#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <osg/Matrixf>
#include <osg/Node>

#include "camera.hpp"
#include "emitterresolver.hpp"
#include "extractionstats.hpp"
#include "lightbuilder.hpp"
#include "materialresolver.hpp"
#include "meantexels.hpp"
#include "mesh.hpp"
#include "meshreader.hpp"
#include "meshresolver.hpp"
#include "mirroridentity.hpp"
#include "mirrorpass.hpp"
#include "nodekind.hpp"
#include "runs.hpp"
#include "sceneadopter.hpp"
#include "scenedesc.hpp"
#include "shading.hpp"
#include "stepped.hpp"

namespace osg
{
    class Drawable;
    class Geometry;
    class Image;
    class StateSet;
}

namespace osgParticle
{
    class ParticleSystem;
}

namespace SceneUtil
{
    class LightSource;
    class MorphGeometry;
    class StateSetUpdater;
}

namespace Rtx
{
    class CellRing;
    class MirrorTraversal;

    /// Mirrors an OpenSceneGraph subtree into a `SceneDesc`. The identity maps live across calls,
    /// so the same geometry met again resolves to the mesh already uploaded rather than to a copy,
    /// and the mirror is incremental instead of a rebuild per frame.
    class SceneExtractor : public SceneAdopter
    {
    public:
        /// @param traversals where this walk's traversal numbers come from. Shared by everything
        ///        that can reach one graph — the game hands the same counter to the world's walk
        ///        and to every traced view. Left out, the extractor keeps a sequence of its own,
        ///        which is right for a harness where nothing else walks the same nodes.
        explicit SceneExtractor(SceneDesc& scene, Traversals* traversals = nullptr);

        /// Out of line because `MirrorTraversal` and the identity maps' key types are only forward
        /// declared here.
        ~SceneExtractor();

        /// Which nodes the walks may descend into, as an `osg` traversal mask: what keeps the
        /// mirror out of subtrees the ray tracer answers for itself, such as the sky's mask,
        /// without knowing what a sky is, and out of what the content hides.
        ///
        /// **Every node, until an owner states less.** The bit a hidden node carries is the
        /// engine's own and this component cannot name it, so a default that subtracted it had to
        /// ask `NifOsg::Loader` at the moment the extractor was built — an answer that depends on
        /// whether anything has told the loader yet, and in the game nothing has. A default that is
        /// plainly everything is wrong where it shows rather than wrong where it does not.
        void setTraversalMask(osg::Node::NodeMask mask) { mTraversalMask = mask; }
        osg::Node::NodeMask getTraversalMask() const { return mTraversalMask; }

        /// Which nodes are the world's water, as an `osg` node mask. None by default. Water reaches
        /// here as an ordinary blended quad, so without this it is shaded as a painted surface:
        /// every shallow goes black under a shadow ray and there are no waves or caustics. A
        /// drawable is water when its own mask carries no bit outside this one, because a node
        /// mask defaults to all ones. The harness places an analytic sea of its own (`addWater`).
        void setWaterMask(osg::Node::NodeMask mask) { mWaterMask = mask; }

        /// Names the node mask the game puts on the root of a class of thing — an actor, an effect,
        /// the player's own arms in first person — by the same rule as the water's: a node whose
        /// mask carries no bit outside this one. Everything under such a node is placed as that
        /// class, for a camera's cull mask to keep or leave out (`InstanceClass`). The harness names
        /// nothing here, so everything it walks is `Static`.
        void setClassMask(InstanceClass what, osg::Node::NodeMask mask);

        /// The class a node carrying `mask` states, or nothing where it states none. Asked by the
        /// walk at every node, which is why it is not the drawable's own question like water.
        std::optional<InstanceClass> classOf(osg::Node::NodeMask mask) const;

        /// How far under a walk's root a `SceneUtil::StableIdentity` is looked for: nought is the
        /// root alone, which is what a caller that stamps nothing wants. The game stamps the
        /// scene root's children and grandchildren — a cell root, and a reference root under it —
        /// and says two; a look deeper than the stamps go is a line of every node loaded for
        /// nothing.
        void setStampDepth(unsigned int depth) { mStampDepth = depth; }
        unsigned int getStampDepth() const { return mStampDepth; }

        /// Where the walks that follow are looked at from, for a billboard to face: the camera's
        /// own basis, `viewBasisOf` its inverse view. Nothing, which is what a fresh extractor
        /// holds, leaves a billboard at its base rotation.
        ///
        /// **Told per walk and read per billboard, because a `NifOsg::AutoTransform` turns only
        /// under a cull visitor** — `computeMatrix` asks the visitor for a `CullStack` and takes
        /// its last rotation otherwise — and no walk in this renderer is one. What a cull hands it
        /// is the eye, the look and the up in the node's own frame, and that is what the walk
        /// hands `computeMatrixForFrame` instead.
        void setEye(const std::optional<ViewBasis>& eye) { mEye = eye; }
        const std::optional<ViewBasis>& getEye() const { return mEye; }

        /// The world's clock, in seconds, which everything the graph animates is driven by.
        /// `SceneUtil::FrameTimeSource` reads the simulation time off the visitor's frame stamp, so
        /// a mirror with a clock of its own would run the game's fires while the game is paused.
        void setSimulationTime(double seconds);

        /// Moves the emitters on by `elapsed` seconds, once per frame. Separate from the world's
        /// clock and only ever forwards: `osgParticle` integrates the gap between one frame stamp
        /// and the last, so a loading screen or a paused window would put every plume in the cell
        /// on its own ceiling at once. Also the sequence every emitter's once-per-frame guard is
        /// kept against, so however many walks reach one, exactly one of them steps it.
        void advanceEmitters(double elapsed);

        /// Walks `node` and places what it finds by `transform`, under `anchor`. A subtree, and it
        /// never reaches the ring: the precipitation node would otherwise place the ground a
        /// second time. `extractWorld` is the call that means the whole of it.
        ///
        /// @param anchor what the caller is placing, stable for as long as it stands. A node path
        ///        does not identify a placement on its own: OpenMW hands out one template node per
        ///        model, and a hundred crates are a hundred calls on that node differing only in
        ///        `transform`. A caller that walks one whole graph can pass zero.
        /// @param frame the game's own, which is what tells a semi-active `SceneUtil::Skeleton` it
        ///        was reached. A caller with no actors in its graph can leave it.
        ExtractionStats extract(
            const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame = 0);

        /// The same, for the walk that is the whole world — the one `retire` is sound after — with
        /// what the graph does not parent walked inside it: the cell ring's ground, statics and
        /// lamps, which have no node anywhere. A reference and not a stored pointer, so no caller
        /// can be the one that forgets it and has the distant ground swept on every frame after the
        /// first, and the extractor holds nothing of the ring between two walks.
        ///
        /// **`ring` is told `frame` here**, because this is the call that holds both: a ring told
        /// one frame and walked for another adopts twice on a frame walked twice, and a caller
        /// that has to remember two calls is a caller that can forget one.
        ExtractionStats extractWorld(const osg::Node& root, const osg::Matrixf& transform, std::size_t anchor,
            std::size_t frame, CellRing& ring);

        /// `extract`, for what falls from the sky: every emitter met under `node` is placed as one
        /// whose sprites a roof keeps off — `MirrorPass::mFalls`. The precipitation's walk and
        /// nothing else, because a hearth's smoke under a roof is where it belongs.
        ExtractionStats extractFalling(
            const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame = 0);

        /// Where this walk's traversal numbers come from — the one handed in, or its own.
        Traversals& getTraversals() { return mTraversals; }

        /// Lets go of everything the walks stood and the ring held, for a world that is detached:
        /// the ring's holds are given back — the two releases read nothing of a walk — and then a
        /// retire at a fresh epoch, which keeps nothing but what is held, because a walk that will
        /// not happen has met nothing. What is left of the world in the scene is nothing, which
        /// `SceneDesc::isEmpty` says. Between walks, and the ring already told of no world.
        Retirement detach(CellRing& ring);

        /// Drops everything the walks since the last call did not find — placements included — and
        /// compacts the scene. Mark and sweep, so only sound where the walks were the whole of what
        /// the scene is of, which every caller's are: the world's frame and a picture's subject
        /// are each re-walked whole. Also the only thing that lets go: the identity maps own their
        /// keys, so a caller that never sweeps holds every drawable it has ever walked.
        ///
        /// **Between the last walk and the hand-over, which the scene enforces.** Handed over
        /// before this, the scene still holds what the walk stopped finding, where the last frame
        /// left it: `SceneDesc::noteWalked` marks the scene at every walk and this is what clears
        /// the mark, and a hand-over of a marked scene is a call out of its turn.
        Retirement retire();

        /// Places one light. The graph and not the content files, because that is where a light
        /// that moves with the thing carrying it exists: a torch in an NPC's hand is no cell
        /// record, and neither is a lamp something picked up and put down.
        void addLight(const SceneUtil::LightSource& source, const osg::Matrixf& place, double simulationTime);

        /// Opens the glow of a magic effect the walk has entered, and closes it where the walk
        /// leaves it — `Rtx::Glow`. One at a time, because the walk is inside one effect at a time:
        /// an effect stated under an effect is the outer one's. Closed and not yet a lamp, because
        /// the effect's flames are read after the walk, with the other emitters; `walk` makes the
        /// lamps once they are.
        void openGlow();
        void closeGlow();

        /// Resolves one drawable and places it. The visitor's whole contract with this class.
        /// `place` is handed over rather than worked out from `path`, because
        /// `osg::computeLocalToWorld` rebuilds the whole chain from the root for every drawable. A
        /// drawable and not an `osg::Geometry`, because a skinned body is an `osg::Drawable` over
        /// a source geometry — the bind pose — beside the rig that poses it.
        void addDrawable(const osg::Drawable& drawable, std::size_t who, std::span<const Shading> shading,
            const osg::Matrixf& place, InstanceClass what);

        /// The state set a node shades with where that is not the one it wears, or null where it
        /// is — `MaterialResolver::animate`, which says what the two cases are. Applied here rather
        /// than left to a callback: a `SceneUtil::StateSetUpdater` as a cull callback writes a state
        /// set that exists only inside a cull traversal, and as an update callback alternates the
        /// node's own between two copies, so a material keyed on the address is added and swept
        /// once a frame. One state set per node, rewritten in place, keeps the address stable.
        ///
        /// @param underAnimated whether an animated state set stands above `node` on the chain,
        ///        which makes a node with a state set of its own animated too.
        const osg::StateSet* animate(osg::Node& node, bool underAnimated);

    private:
        /// Where the extractor stands: between walks, or inside one. A walk inside a walk would
        /// point the pass at a second set of counts and lose the first's, and a retire inside one
        /// would sweep what the walk is about to stamp; both are asserted where they happen.
        enum class Phase
        {
            Between,
            Walking,
        };

        /// The pass opened for one walk and closed however the walk ends: the counts pointer back
        /// to null, the falls flag cleared and the phase back to `Between`, on the ordinary return
        /// and on a throw alike. A resolver reached after a walk that threw would otherwise count
        /// into an unwound local.
        class WalkGuard
        {
        public:
            WalkGuard(MirrorPass& pass, Stepped<Phase>& phase, ExtractionStats& stats, bool falls);
            ~WalkGuard();

            WalkGuard(const WalkGuard&) = delete;
            WalkGuard& operator=(const WalkGuard&) = delete;

        private:
            MirrorPass& mPass;
            Stepped<Phase>& mPhase;
        };

        /// What the ring may do, and nothing else may. `Rtx::SceneAdopter` is implemented
        /// privately, so its four calls are reachable through that interface and not in front of
        /// every reader of this class. The two adoptions mean anything only inside a walk; the
        /// two releases are allowed between walks as well — `detach` — because giving a hold
        /// back reads nothing of a walk.
        Index adoptMesh(const osg::Drawable& drawable, const MeshReading& reading) override
        {
            mPhase.expect(Phase::Walking);
            return mMeshes.adopt(drawable, reading);
        }
        Index adoptMaterial(const MaterialReading& reading) override
        {
            mPhase.expect(Phase::Walking);
            return mMaterials.adopt(reading);
        }
        void releaseMesh(const osg::Drawable& drawable) override { mMeshes.release(drawable); }
        void releaseMaterial(const osg::StateSet* key) override { mMaterials.release(key); }

        /// Whether a drawable carrying `mask` is the world's water.
        bool isWater(osg::Node::NodeMask mask) const;

        /// Whether `mask` carries no bit outside `named`, which is what both questions above ask.
        static bool carriesOnly(osg::Node::NodeMask mask, osg::Node::NodeMask named);

        /// What the three walks are, differing only in whether the ring's geometry is asked for
        /// and whether what is placed falls.
        ExtractionStats walk(const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor,
            std::size_t frame, CellRing* ring, bool falls);

        SceneDesc& mScene;

        /// What kind each class this walk meets is, node or drawable. A member because the
        /// answers are a fact about the classes in the world rather than about one frame, and
        /// before the walk, which reads it.
        NodeKinds mKinds;

        /// The walk itself, made once rather than per call, because the chain of state sets it
        /// refills as it descends would be a per-frame allocation as a local.
        std::unique_ptr<MirrorTraversal> mWalk;

        /// Used only where the caller named none.
        Traversals mOwnTraversals;
        Traversals& mTraversals;

        /// Every node until the owner states a mask (`setTraversalMask`).
        osg::Node::NodeMask mTraversalMask;

        /// See `setStampDepth`.
        unsigned int mStampDepth = 0;

        /// Which drawables are the sea. Zero means none of them, which is every caller that has not
        /// said otherwise.
        osg::Node::NodeMask mWaterMask = 0;

        /// The root mask of each class but `Static`, which is what a node states none of. Zero
        /// means the class is never stated, which is every caller that has not said otherwise.
        struct ClassMask
        {
            InstanceClass mClass;
            osg::Node::NodeMask mMask = 0;
        };
        std::array<ClassMask, 3> mClassMasks{ ClassMask{ InstanceClass::Actor }, ClassMask{ InstanceClass::Effect },
            ClassMask{ InstanceClass::FirstPerson } };

        /// See `setEye`.
        std::optional<ViewBasis> mEye;

        /// Every effect this walk entered, in the order it entered them, and which of them it is
        /// inside, where it is inside one. Reserved once, `sEffectBudget`.
        std::vector<Glow> mGlows;
        std::optional<std::size_t> mGlow;

        /// The mean texel of every additive map met, for the process: a sheet's and a flame's
        /// alike, so the two resolvers below share it.
        MeanTexels mMeans;

        /// Which sweep is current, and where the walk in progress puts its counts. Declared before
        /// the walk and every resolver below, which borrow it rather than keep a copy that could
        /// fall behind.
        MirrorPass mPass;

        Stepped<Phase> mPhase{ Phase::Between };

        /// Which slot each placement holds, and when it was last met. One lookup a placement a
        /// frame, and the scene keeps the transform.
        Kept<std::unordered_map<std::size_t, Known>> mPlacements{ mPass };

        /// The drawables the walk met, and what poses the ones that deform.
        MeshResolver mMeshes{ mScene, mPass };

        /// What the content says each surface is, and the textures those name.
        MaterialResolver mMaterials{ mScene, mPass, mMeans };

        /// The particle systems the walk met, and the sprite textures they hold.
        EmitterResolver mEmitters{ mScene, mPass, mMeans };

        // Refilled per sweep: the survivors, as the scene wants them.
        std::vector<Index> mLiveMeshes;
        std::vector<Index> mLiveMaterials;
    };
}
