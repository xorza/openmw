#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec3f>

#include "emitterresolver.hpp"
#include "extractionstats.hpp"
#include "materialresolver.hpp"
#include "meshresolver.hpp"
#include "mirrorpass.hpp"
#include "scenedesc.hpp"
#include "shading.hpp"
#include "traversals.hpp"

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

namespace Terrain
{
    class TerrainDrawable;
}

namespace Rtx
{
    class MirrorTraversal;

    /// What a walk of the scene graph cannot reach, offered to the walk that asks for it.
    ///
    /// **`Terrain::QuadTreeWorld` is the reason this exists.** With `distant terrain` on it resolves
    /// its chunks inside a cull, against a view keyed on the camera culling, and parents them to
    /// nothing — so the ground, the paged objects and the grass are invisible to any visitor that is
    /// not a cull, which is every visitor a ray tracer has. It cannot be made a cull either: a cull
    /// puts a chunk in a render bin instead of applying it, so walking the graph that way makes the
    /// ground vanish rather than appear.
    ///
    /// So it is asked instead of walked, and this is the shape of the question. `TerrainResidency`
    /// stands the chunks. **What comes back is not only geometry**: `DistantLights` stands the lamps
    /// of the cells the paging leaves dark, which have no node in either renderer because `LIGH` is
    /// not a paged type. The abstraction is here because the extractor may be handed none, which is
    /// every world that parents its chunks like anything else.
    ///
    /// **One method, because a host holds each of these as itself and not through this.** What a
    /// residency has to be told differs by what it stands — a terrain wants the eye, distant lights
    /// want the eye, the reach and the active grid — so the setters stay on the classes and only
    /// the asking is shared. Hoisting the one setter both happen to have would leave a host
    /// reaching the rest by name anyway, and a reader wondering why the eye arrived by a different
    /// route than the grid.
    class Residency
    {
    public:
        virtual ~Residency() = default;

        /// Hands `visitor` everything held that the graph does not parent.
        virtual void collect(osg::NodeVisitor& visitor) = 0;
    };

    /// Mirrors an OpenSceneGraph subtree into a `SceneDesc`.
    ///
    /// The identity maps live across calls, so the same geometry met again — in another cell, under
    /// another reference, in a later frame — resolves to the mesh already uploaded rather than to a
    /// copy of it. That is what makes an incremental mirror possible instead of a rebuild per frame,
    /// and it is why this is an object rather than a function.
    class SceneExtractor
    {
    public:
        /// @param traversals where this walk's traversal numbers come from. **Shared by everything
        ///        that can reach one graph** — the game hands the same counter to the world's walk
        ///        and to every traced view. Left out, the extractor keeps a sequence of its own,
        ///        which is right for a harness where nothing else walks the same nodes.
        explicit SceneExtractor(SceneDesc& scene, Traversals* traversals = nullptr);

        /// Out of line because `MirrorTraversal` and the identity maps' key types are only forward
        /// declared here.
        ~SceneExtractor();

        SceneExtractor(const SceneExtractor&) = delete;
        SceneExtractor& operator=(const SceneExtractor&) = delete;

        /// Which nodes the walks may descend into, as an `osg` traversal mask.
        ///
        /// **What keeps the mirror out of subtrees the ray tracer answers for itself.** The engine
        /// already marks them — the sky is `SceneUtil::Mask_Sky` — and a mask is how OSG is asked
        /// to skip one, so nothing here has to know what a sky is.
        ///
        /// Everything the content did not hide, by default. A host with more to leave out says so,
        /// and says it with the hidden bit still out — see the constructor.
        void setTraversalMask(osg::Node::NodeMask mask) { mTraversalMask = mask; }

        /// Which nodes are the world's water, as an `osg` node mask. None by default.
        ///
        /// **The engine already marks it and the mirror could not tell otherwise.** Water reaches
        /// here as an ordinary blended quad with a texture on it, and nothing about the geometry or
        /// the state set says it is a sea — so without this it is shaded as a painted surface: no
        /// `MASK_WATER`, so a shadow ray stops at the surface and every shallow in the game goes
        /// black; and no waves, refraction or caustics, which are what the renderer has for it.
        ///
        /// A drawable is water when its own mask carries **no bit outside** this one — not merely a
        /// bit inside it. A node mask defaults to all ones, so an intersection test calls every
        /// drawable that never set one the sea. The harness names nothing here because it places an
        /// analytic sea of its own (`addWater`).
        void setWaterMask(osg::Node::NodeMask mask) { mWaterMask = mask; }

        /// Names the node mask the game puts on the player's own arms in first person, by the same
        /// rule as the water's: a node whose mask carries no bit outside this one. Everything under
        /// such a node is placed for the eye alone — `Shaders::MASK_FIRST_PERSON` says why. The
        /// harness names nothing here, since it never stands in first person.
        void setFirstPersonMask(osg::Node::NodeMask mask) { mFirstPersonMask = mask; }

        /// Whether a node carrying `mask` is the root of the player's first-person arms. Asked by
        /// the walk at every node, which is why it is not the drawable's own question like water.
        bool isFirstPerson(osg::Node::NodeMask mask) const;

        /// The world's clock, in seconds, which everything the graph animates is driven by.
        ///
        /// **The world's and not the walk's.** `SceneUtil::FrameTimeSource` — what `NifOsg` gives
        /// every controller it finds no other source for — reads the simulation time straight off
        /// the visitor's frame stamp, so a mirror with a clock of its own would run the game's
        /// fires at its own frame rate and go on running them while the game is paused.
        void setSimulationTime(double seconds);

        /// Moves the emitters on by `elapsed` seconds, and must be called once per frame.
        ///
        /// **Separate from the world's clock, and only ever forwards.** `osgParticle` integrates the
        /// gap between one frame stamp and the last, so it is the one thing here that cannot be
        /// handed an absolute time: a loading screen, a paused window or a harness warming its
        /// emitters up while the world stands still are each a gap that would put every plume in the
        /// cell on its own ceiling at once. A step backwards or a jump is clamped away here rather
        /// than guarded against at each call.
        ///
        /// This is also the sequence every emitter's once-per-frame guard is kept against, so
        /// however many walks reach one, exactly one of them steps it.
        void advanceEmitters(double elapsed);

        /// Runs the emitters under `node` without mirroring anything, on the clock above.
        ///
        /// For a caller that needs them somewhere other than where a file seeded them before it has
        /// a frame to show — a harness warming a cell's candles up so the first shot has flames in
        /// it. A walk that mirrors runs them as it goes and does not need this.
        void stepEmitters(osg::Node& node);

        /// Walks `node` and places what it finds by `transform`, under `anchor`.
        ///
        /// Takes a const reference because nothing here writes to the graph; OSG's visitor API is
        /// non-const throughout regardless, so the cast happens once, here.
        ///
        /// @param anchor what the caller is placing, stable for as long as it stands. **A node path
        ///        does not identify a placement on its own**, because the same subtree is walked
        ///        under many of them: OpenMW hands out one template node per model and a hundred
        ///        crates are a hundred calls on that same node, all with the same path and all
        ///        differing only in the `transform` given here. Anything the caller can keep is a
        ///        good anchor — a reference id, an actor's address, a terrain chunk — and a caller
        ///        that walks one whole graph, where every path is already distinct, can pass zero.
        /// @param frame the game's own, which is what tells a semi-active `SceneUtil::Skeleton` it
        ///        was reached. A caller with no actors in its graph can leave it.
        ///
        /// **A subtree, and it never reaches the residency.** What hides its geometry is a property
        /// of the world and not of any node under it, so a walk that starts part way down must not
        /// bring it in — the precipitation node would otherwise place the ground a second time.
        /// `extractWorld` is the call that means the whole of it.
        ExtractionStats extract(
            const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame = 0);

        /// The same, for the walk that is the whole world — the one `retire` is sound after.
        ///
        /// **The residency comes from `follow` rather than from an argument, and that is the point.**
        /// The sweep is global: anything a walk did not meet is dropped. So a frame walked by two
        /// owners, only one of which remembered to hand over what the graph does not parent, retires
        /// the other's placements — which is how a paged world's chunks reached the mirror on the
        /// first frame and were swept on every one after it, leaving a town standing on open sea.
        /// Held on the extractor, no caller can be the one that forgets.
        ExtractionStats extractWorld(
            const osg::Node& root, const osg::Matrixf& transform, std::size_t anchor, std::size_t frame = 0);

        /// What the graph does not parent, walked with every world walk from here on.
        ///
        /// **A list, because more than one thing keeps its own.** A quad tree resolves its chunks
        /// inside a cull and parents them to nothing; the lights of the cells it pages have no node
        /// anywhere, because the reference that carries one is not a paged type. Each is collected
        /// into the same walk, in the order given.
        ///
        /// Copied, so a caller may hand over a temporary. Empty where nothing hides, which is every
        /// world whose ground is in the graph.
        void follow(std::span<Residency* const> residents) { mResidents.assign(residents.begin(), residents.end()); }

        /// Ends a frame: what was placed becomes what was placed before.
        ///
        /// **Called once per frame by whoever is mirroring a live graph, and never by anything that
        /// walks a world once.** Until it is called, every placement's previous transform is its
        /// current one, which is exactly right for a scene that does not move — and a harness that
        /// loads a region and flies a camera round it wants that answer, not a stale one.
        ///
        /// Freeing the slots of placements that have gone is `retire`'s job and not this one, for
        /// the reason written there: only a caller whose walks were the whole world can tell a
        /// placement that has left the graph from one it simply did not visit.
        void advance();

        /// Drops everything the walks since the last call did not find — placements included — and
        /// compacts the scene.
        ///
        /// **Only where the walks were the whole world.** This is mark and sweep: what makes it
        /// sound is that anything alive was met, so a caller that walks a region once and then
        /// mirrors only the movers would retire the region it is standing in. The game re-walks its
        /// whole graph every frame and can call this; the harness keeps a snapshot and does not.
        ///
        /// **It is also the only thing that lets go.** The identity maps own their keys, which is
        /// what makes an address mean one object for as long as an entry names it; the cost of that
        /// is that geometry the graph has dropped outlives its owner until a sweep, and a caller
        /// that never sweeps holds every drawable it has ever walked.
        ///
        /// Anything a caller kept across this — a snapshot of placements, an index of its own — is
        /// stale afterwards, and `SceneDesc::getStructureRevision` is what says so.
        Retirement retire();

        /// Places one light. **The graph and not the content files**, because that is where a light
        /// that moves with the thing carrying it exists: a torch in an NPC's hand is no cell
        /// record, and neither is a lamp something picked up and put down.
        void addLight(const SceneUtil::LightSource& source, const osg::Matrixf& place, double simulationTime);

        /// Resolves one drawable and places it. The visitor's whole contract with this class.
        ///
        /// `place` is where the drawable stands in the world, which the visitor has accumulated on
        /// its way down; `path` is what identifies the placement and where the state that shades it
        /// comes from. **The transform is handed over rather than worked out from the path**,
        /// because `osg::computeLocalToWorld` rebuilds the whole chain from the root for every
        /// drawable and the visitor already holds the part they share.
        ///
        /// **A drawable and not an `osg::Geometry`**, because a skinned body is neither: it is an
        /// `osg::Drawable` over a source geometry, and what the mirror reads is that source — the
        /// bind pose — beside the rig that poses it. Which of the kinds this is belongs here rather
        /// than to a caller — the visitor would only be asking the same question with less to
        /// answer it from.
        void addDrawable(const osg::Drawable& drawable, std::size_t who, std::span<const Shading> shading,
            const osg::Matrixf& place, bool firstPerson);

        /// The state set a node's controllers write, or null where it has none.
        ///
        /// **Applied here rather than left to a callback.** A `SceneUtil::StateSetUpdater` set as a
        /// cull callback writes into a state set it keys on the visitor and pushes onto that
        /// visitor's stack, so what it produces exists only inside a cull traversal and a mirror
        /// running outside one sees the frame it first met, for ever. Set as an *update* callback it
        /// alternates the node's own state set between two copies of itself, so a material keyed on
        /// that address is added and swept once a frame for a surface that has not moved.
        ///
        /// One state set per node, made once and rewritten in place, answers both: the address is
        /// stable, so the material keeps its slot, and the values are this frame's.
        const osg::StateSet* animate(osg::Node& node);

    private:
        /// Whether a drawable carrying `mask` is the world's water.
        bool isWater(osg::Node::NodeMask mask) const;

        /// Whether `mask` carries no bit outside `named`, which is what both questions above ask.
        static bool carriesOnly(osg::Node::NodeMask mask, osg::Node::NodeMask named);

        /// What both `extract` and `extractWorld` are, differing only in whether the world's hidden
        /// geometry is asked for.
        ExtractionStats walk(const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor,
            std::size_t frame, std::span<Residency* const> hidden);

        SceneDesc& mScene;

        /// Which slot each placement holds, and when it was last met.
        ///
        /// **This is what a slot buys.** The two maps of matrices it replaces were rebuilt every
        /// frame — a lookup, an insert and a heap node for each of fifty thousand placements, to
        /// carry a transform from one frame to the next that the scene can simply keep. What
        /// remains is one lookup, and Phase 2 is about not making that either.
        std::unordered_map<std::size_t, Known> mPlacements;

        /// The walk itself, made once rather than per call.
        ///
        /// It carries the sequence clock, the emitter clock and the chain of state sets it refills
        /// as it descends, and each is a per-frame allocation if the walk is a local — a cell is
        /// tens of thousands of drawables deep.
        std::unique_ptr<MirrorTraversal> mWalk;

        /// Used only where the caller named none.
        Traversals mOwnTraversals;
        Traversals& mTraversals;

        /// Set in the constructor, because the default is asked of the loader rather than named.
        osg::Node::NodeMask mTraversalMask;

        /// Which drawables are the sea. Zero means none of them, which is every caller that has not
        /// said otherwise.
        osg::Node::NodeMask mWaterMask = 0;
        osg::Node::NodeMask mFirstPersonMask = 0;

        /// Geometry no node parents, asked of every world walk. See `follow`.
        /// Refilled by `follow` every frame and never freed: two of them at most, so far.
        std::vector<Residency*> mResidents;

        /// What the walk in progress was told it is placing. See `extract`.
        std::size_t mAnchor = 0;

        /// Which sweep is current, and where the walk in progress puts its counts.
        ///
        /// **Declared before everything that borrows it**, which is the walk and every resolver
        /// below: each reads the mirror's own rather than keeping a copy that could fall behind it.
        MirrorPass mPass;

        /// The drawables the walk met, and what poses the ones that deform.
        MeshResolver mMeshes{ mScene, mPass };

        /// What the content says each surface is, and the textures those name.
        MaterialResolver mMaterials{ mScene, mPass };

        /// The particle systems the walk met, and the sprite textures they hold.
        EmitterResolver mEmitters{ mScene, mPass };

        /// How many placements this epoch's walks stamped, against how many the map holds.
        ///
        /// **What lets the sweep be skipped rather than run to find nothing.** A world that stands
        /// still reaches every placement it holds, so the two agree and there is provably nothing
        /// stale to erase — where the sweep would iterate tens of thousands of entries, a cache miss
        /// apiece, to reach the same conclusion. It is only ever an equality: a walk stamps an entry
        /// once, so the count cannot pass the size, and anything short of it means something in the
        /// map went unreached and the sweep has to run.
        std::size_t mPlacementsReached = 0;

        // Refilled per sweep: the survivors, as the scene wants them.
        std::vector<Index> mLiveMeshes;
        std::vector<Index> mLiveMaterials;
    };
}
