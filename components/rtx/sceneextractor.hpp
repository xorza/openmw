#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include "scenedesc.hpp"

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
    class StateSetUpdater;
}

namespace Terrain
{
    class TerrainDrawable;
}

namespace Rtx
{
    /// What one extraction pass did.
    ///
    /// The reused counts are the interesting half: a mirror that adds nothing on a second pass over
    /// an unchanged graph is the property the whole incremental design rests on, and it is only
    /// visible as a number.
    struct ExtractionStats
    {
        /// Distinct geometry met for the first time, so one new entry in the scene each.
        std::uint32_t mMeshesAdded = 0;
        std::uint32_t mMaterialsAdded = 0;

        /// Ground wide enough that its layer stack is baked into one texture rather than shaded a
        /// layer at a time. Distant chunks and nothing else — see `sCompositeFrom`.
        std::uint32_t mComposites = 0;

        /// Drawables that resolved to something already known. A count of lookups, not of meshes:
        /// a hundred crates sharing one model contribute a hundred here and one above.
        std::uint32_t mMeshesReused = 0;
        std::uint32_t mMaterialsReused = 0;
        std::uint32_t mInstances = 0;

        /// Drawables whose vertices are recomputed every frame and so were read from the pose
        /// rather than from the cache: skinned bodies and morphed faces. Each one already met is a
        /// bottom-level structure a backend has to build again, which is what makes this the cost
        /// of an actor rather than a count of them.
        std::uint32_t mDeformed = 0;

        /// Particle systems met, and the live particles they were holding.
        ///
        /// **Sprites and not triangles**, so neither number is a mesh or an instance: an emitter is
        /// a sphere and a run of discs the primary ray composites, and nothing about it reaches an
        /// acceleration structure. An emitter whose particles have all died places nothing and is
        /// not counted.
        std::uint32_t mEmitters = 0;
        std::uint32_t mSprites = 0;

        /// Drawables this cannot read at all — neither an `osg::Geometry`, nor either of the two
        /// deforming kinds, nor a particle system.
        ///
        /// What is left is OpenMW's own debug drawing, which a ray tracer answers differently
        /// rather than misses. A canary and not a deficit: what it would catch is a new kind of
        /// drawable arriving unnoticed.
        std::uint32_t mSkippedUnknown = 0;

        /// Surfaces the content pipeline never described, which are drawn as whatever a default
        /// `Material` is — untextured, opaque and one-sided.
        ///
        /// **A canary, and it should be zero.** `NifOsg` and `Terrain` author a `Surface::Material`
        /// for everything they build; a drawable arriving without one means a state set was made
        /// somewhere else, or remade by something that copied the pipeline state and dropped the
        /// description with it.
        std::uint32_t mUndescribedMaterials = 0;

        /// What the textures a scene reached for turned out to be.
        ///
        /// Kept because the answer decides how they are uploaded, and guessing it from what the
        /// content files ought to contain is how a renderer ends up with a path nothing takes.
        std::map<std::string, std::uint32_t> mTextureFormats;

        /// Geometry with no vertices or no triangles. Morrowind ships some.
        std::uint32_t mSkippedEmpty = 0;

        /// Lamps taken off the graph. Zero in the harness, which has no `LightManager` and reads
        /// the cell's `LIGH` records instead.
        std::uint32_t mLights = 0;

        ExtractionStats& operator+=(const ExtractionStats& other);
    };

    /// What one sweep dropped.
    struct Retirement
    {
        std::uint32_t mMeshes = 0;
        std::uint32_t mMaterials = 0;

        bool empty() const { return mMeshes == 0 && mMaterials == 0; }
    };

    class MirrorTraversal;

    /// The numbers mirror walks pose at, and the rule that they only ever go up.
    ///
    /// **`SceneUtil::Skeleton` and both deforming drawables refuse to move for a traversal number
    /// they have already seen.** That is what stops one actor being skinned twice in a frame, and it
    /// means a walk's number is not a label but a claim: this pose is newer than the last.
    ///
    /// This fork has two things that walk — the world, once a frame, and a traced view whenever its
    /// subject changes — and they must not be two sequences. A subtree reached by both would be posed
    /// by whichever got there first and frozen for the other, and nothing states that no subtree is
    /// shared: `NpcAnimation` merely happens to clone a `RigGeometry` per instance. One counter, and
    /// the hazard cannot arise.
    ///
    /// **Not the frame number**, which a walk also carries and which means something else — which of
    /// a `SceneUtil::LightSource`'s two buffers update has just written. A doll redrawn twice in one
    /// frame needs two pose numbers and one light buffer.
    class Traversals
    {
    public:
        /// The next number, greater than every number handed out before it.
        unsigned int next() { return ++mLast; }

    private:
        /// **From one and not from zero.** Everything OSG poses starts at a traversal number of
        /// zero, so a first walk saying zero is a walk that poses nothing.
        unsigned int mLast = 0;
    };

    /// Geometry a walk of the scene graph cannot reach, offered to the walk that asks for it.
    ///
    /// **`Terrain::QuadTreeWorld` is the reason this exists.** With `distant terrain` on it resolves
    /// its chunks inside a cull, against a view keyed on the camera culling, and parents them to
    /// nothing — so the ground, the paged objects and the grass are invisible to any visitor that is
    /// not a cull, which is every visitor a ray tracer has. It cannot be made a cull either: a cull
    /// puts a chunk in a render bin instead of applying it, so walking the graph that way makes the
    /// ground vanish rather than appear.
    ///
    /// So it is asked instead of walked, and this is the shape of the question. `TerrainResidency`
    /// is the one implementation; the abstraction is here because the extractor may be handed none,
    /// which is every world that parents its chunks like anything else.
    class Residency
    {
    public:
        virtual ~Residency() = default;

        /// Hands `visitor` everything held that the graph does not parent.
        virtual void collect(osg::NodeVisitor& visitor) = 0;
    };

    /// Hashes and compares an owning key by the address it holds.
    ///
    /// **What lets an identity map hold its subject alive without paying for that on a lookup.** A
    /// map keyed on a raw `osg` pointer can be fooled: the engine frees a body part and the
    /// allocator puts the replacement exactly where it was, so the walk that meets the new one finds
    /// the old one's entry and mirrors geometry it has nothing to do with. A `ref_ptr` key makes the
    /// address *true* — nothing else can hold it while the entry does — and being transparent is
    /// what keeps every lookup from a raw pointer out of the reference count.
    template <class T>
    struct ByAddress
    {
        using is_transparent = void;

        std::size_t operator()(const osg::ref_ptr<T>& value) const { return std::hash<const T*>{}(value.get()); }
        std::size_t operator()(const T* value) const { return std::hash<const T*>{}(value); }

        bool operator()(const osg::ref_ptr<T>& left, const osg::ref_ptr<T>& right) const
        {
            return left.get() == right.get();
        }
        bool operator()(const osg::ref_ptr<T>& left, const T* right) const { return left.get() == right; }
        bool operator()(const T* left, const osg::ref_ptr<T>& right) const { return left == right.get(); }
    };

    /// One state set in the chain that shades a drawable, nearest it last.
    ///
    /// **Not simply a node's own state set.** OpenMW animates shading by handing a
    /// `SceneUtil::StateSetUpdater` a state set that belongs to the traversal rather than to the
    /// graph — that is how a fire flips through its frames and how lava scrolls — so the state set
    /// in force at a drawable is something a walk builds and not something a node holds.
    struct Shading
    {
        const osg::StateSet* mStateSet = nullptr;

        /// Whether a controller rewrote this since the last frame, so the material read from it is
        /// not the material it will be next frame. What tells `resolveMaterial` to read a known
        /// state set again instead of handing back the slot it already has.
        bool mAnimated = false;
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
        /// @param traversals where this walk's pose numbers come from. **Shared by everything that
        ///        can reach one graph** — the game hands the same counter to the world's walk and to
        ///        every traced view. Left out, the extractor keeps a sequence of its own, which is
        ///        right for a harness where nothing else walks the same nodes.
        explicit SceneExtractor(SceneDesc& scene, Traversals* traversals = nullptr);

        /// Out of line because `MirrorTraversal` and the identity maps' key types are only forward
        /// declared here.
        ~SceneExtractor();

        SceneExtractor(const SceneExtractor&) = delete;
        SceneExtractor& operator=(const SceneExtractor&) = delete;

        /// Which nodes the walks may descend into, as an `osg` traversal mask.
        ///
        /// **What keeps the mirror out of subtrees the ray tracer answers for itself.** The engine
        /// already marks them — OpenMW's sky is `Mask_Sky` — and a mask is how OSG is asked to skip
        /// one, so nothing here has to know what a sky is. Everything by default.
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
        /// @param frame which of a `SceneUtil::LightSource`'s two buffers to read. The game passes
        ///        the viewer's frame number, which is the one update has just finished writing;
        ///        anything with no `LightManager` in its graph can leave it.
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
        /// Null where nothing hides, which is every world whose ground is in the graph.
        void follow(Residency* resident) { mResident = resident; }

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
        /// stale afterwards, and `SceneDesc::getRevision` is what says so.
        Retirement retire();

        /// Places one light. **The graph and not the content files**, because that is where a light
        /// that moves with the thing carrying it exists: a torch in an NPC's hand is no cell
        /// record, and neither is a lamp something picked up and put down.
        void addLight(
            const SceneUtil::LightSource& source, const osg::Matrixf& place, std::size_t frame, ExtractionStats& stats);

        /// Resolves one drawable and places it. The visitor's whole contract with this class.
        ///
        /// `place` is where the drawable stands in the world, which the visitor has accumulated on
        /// its way down; `path` is what identifies the placement and where the state that shades it
        /// comes from. **The transform is handed over rather than worked out from the path**,
        /// because `osg::computeLocalToWorld` rebuilds the whole chain from the root for every
        /// drawable and the visitor already holds the part they share.
        ///
        /// **A drawable and not an `osg::Geometry`**, because a skinned body is neither: it is an
        /// `osg::Drawable` holding two internal geometries and writing the pose the cull traversal
        /// just computed into whichever of them was not last drawn. Which of the kinds this is
        /// belongs here rather than to a caller — the visitor would only be asking the same
        /// question with less to answer it from.
        void addDrawable(const osg::Drawable& drawable, const osg::NodePath& path, std::span<const Shading> shading,
            const osg::Matrixf& place, ExtractionStats& stats);

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
        /// Places one particle system's live particles as a run of camera-facing discs.
        ///
        /// **The engine's own simulation, read where it stands.** OpenMW runs `osgParticle` under
        /// the update traversal — emitters, colliders, gravity, colour and size ramps, the lot —
        /// so by the time the mirror walks the graph a flame is a list of positions, sizes and
        /// colours. Re-deriving that from the `NiParticleSystemController` would be a second
        /// implementation of the same content, free to disagree with the one the game is running.
        /// An emitter the walk met, waiting for the walk to finish before its particles are read.
        struct PendingEmitter
        {
            const osgParticle::ParticleSystem* mParticles;
            osg::Matrixf mPlace;
            Index mTexture;
            bool mLight;

            /// Kept only to name the texture's format in the stats, which is read once per emitter.
            const osg::Image* mSprite;
        };

        /// Reads every emitter the walk collected, now that everything in it has been stepped.
        void flushEmitters(ExtractionStats& stats);

        /// Reads one of them into the scene.
        void placeSprites(const PendingEmitter& pending, ExtractionStats& stats);

        void addEmitter(const osgParticle::ParticleSystem& particles, std::span<const Shading> shading,
            const osg::Matrixf& place, ExtractionStats& stats);

        /// What identifies one placement from one frame to the next.
        ///
        /// **The anchor and the node path under it, hashed together.** Neither is enough alone: a
        /// drawable is not an instance, because a hundred crates share one geometry, and a path is
        /// not one either, because a hundred crates walked from a shared template node share the
        /// path as well. What tells them apart is what the caller was placing.
        ///
        /// Hashed rather than kept, because a path is a vector of pointers per placement and the
        /// map is walked every frame; at sixty-four bits over tens of thousands of placements a
        /// collision is not a thing that happens.
        static std::size_t identify(std::size_t anchor, const osg::NodePath& path);

        /// The mesh index for one drawable, adding it or re-reading it as its kind requires.
        ///
        /// **Keyed on the drawable and not on the geometry**, because a deforming drawable's
        /// geometry pointer alternates between its two buffers: keying on that would put two frozen
        /// poses of every actor in the scene and flicker between them.
        Index resolveMesh(
            const osg::Drawable& drawable, const osg::Geometry& geometry, bool deforming, ExtractionStats& stats);
        /// Whether a drawable carrying `mask` is the world's water.
        bool isWater(osg::Node::NodeMask mask) const;

        /// The one material every water drawable wears, made on demand.
        Index resolveWaterMaterial(ExtractionStats& stats);

        Index resolveMaterial(std::span<const Shading> shading, ExtractionStats& stats);

        /// Adds a described texture to the scene, or nothing where the role is unfilled.
        Index takeTexture(const osg::Image* image, ExtractionStats& stats);

        /// Reads a material off the description in force, without asking whether one is known.
        Material readMaterial(std::span<const Shading> shading, ExtractionStats& stats);

        /// The layered material of a terrain chunk, whose shading is not on the graph at all.
        ///
        /// `Terrain::TerrainDrawable` carries one pass per ground texture, each alpha-blended over
        /// the last across the same triangles, because that is how a rasterizer draws a blend it
        /// cannot sample in one go. A ray tracer hits the ground once and shades it once, so the
        /// passes are read back into layers and summed there instead.
        /// What both `extract` and `extractWorld` are, differing only in whether the world's hidden
        /// geometry is asked for.
        ExtractionStats walk(const osg::Node& node, const osg::Matrixf& transform, std::size_t anchor,
            std::size_t frame, Residency* hidden);

        Index resolveTerrainMaterial(const Terrain::TerrainDrawable& terrain, ExtractionStats& stats);

        SceneDesc& mScene;

        /// An entry in one of the identity maps, and when it was last met.
        ///
        /// The epoch is what `retire` sweeps on: a walk stamps everything it resolves, so anything
        /// still carrying an older stamp is something the graph no longer has.
        struct Known
        {
            Index mIndex = sNoIndex;
            std::uint64_t mEpoch = 0;
        };

        /// Which slot each placement holds, and when it was last met.
        ///
        /// **This is what a slot buys.** The two maps of matrices it replaces were rebuilt every
        /// frame — a lookup, an insert and a heap node for each of fifty thousand placements, to
        /// carry a transform from one frame to the next that the scene can simply keep. What
        /// remains is one lookup, and Phase 2 is about not making that either.
        std::unordered_map<std::size_t, Known> mPlacements;

        /// What the scene knows one `osg` object as, keyed so the object cannot go while the entry
        /// stands. See `ByAddress`.
        template <class T>
        using Identity = std::unordered_map<osg::ref_ptr<T>, Known, ByAddress<T>, ByAddress<T>>;

        // Keyed on pointer identity, which OpenMW's resource cache and its optimizer's
        // SHARE_DUPLICATE_STATE pass together make meaningful: the same model loaded twice is the
        // same object, and equivalent state sets are collapsed into one.
        //
        // **Owning, which is what makes that identity sound.** What these hold outlives the graph
        // by one sweep, and a sweep is what lets go.
        Identity<const osg::Drawable> mMeshes;
        Identity<const osg::StateSet> mMaterials;

        /// Which texture each particle system draws with.
        ///
        /// **This entry is the reference**, and not a note about one: a sprite's texture hangs off
        /// no material, so the scene is told to hold it when the emitter is first met and to let go
        /// when the sweep loses it. It saves a path hash per emitter per frame as well.
        Identity<const osg::Drawable> mEmitterTextures;

        /// A node's controllers and the state set they write into, kept so the address is the same
        /// one next frame. See `animate`.
        struct Animated
        {
            osg::ref_ptr<osg::StateSet> mStateSet;
            std::uint64_t mEpoch = 0;
        };

        /// Owning for the same reason the identity maps are: a node freed and replaced at the same
        /// address would otherwise be handed the state set the first one's controllers were writing.
        std::unordered_map<osg::ref_ptr<const osg::Node>, Animated, ByAddress<const osg::Node>,
            ByAddress<const osg::Node>>
            mAnimated;

        /// The walk itself, made once rather than per call.
        ///
        /// It carries the pose traversal, whose state graph, render stage, viewport and matrices are
        /// set up in its constructor, and the chain of state sets it refills as it descends. Both
        /// are per-frame allocations if the walk is a local, and a cell is tens of thousands of
        /// drawables deep.
        std::unique_ptr<MirrorTraversal> mWalk;

        /// Used only where the caller named none.
        Traversals mOwnTraversals;
        Traversals& mTraversals;

        osg::Node::NodeMask mTraversalMask = ~0u;

        /// Which drawables are the sea. Zero means none of them, which is every caller that has not
        /// said otherwise.
        osg::Node::NodeMask mWaterMask = 0;

        /// Geometry no node parents, asked of every world walk. See `follow`.
        Residency* mResident = nullptr;

        /// The sea's material and when it was last met. Not in `mMaterials`, because what identifies
        /// it is the node mask rather than any state set — see `resolveWaterMaterial`.
        Index mWaterMaterial = sNoIndex;
        std::uint64_t mWaterEpoch = 0;

        /// What the walk in progress was told it is placing. See `extract`.
        std::size_t mAnchor = 0;

        /// Which sweep is current. Everything a walk resolves or places is stamped with it.
        std::uint64_t mEpoch = 0;

        // Refilled per sweep: the survivors, as the scene wants them.
        std::vector<Index> mLiveMeshes;
        std::vector<Index> mLiveMaterials;

        // Refilled per drawable rather than reallocated, because a cell is tens of thousands of them.
        std::vector<std::uint32_t> mIndexScratch;
        std::vector<osg::Vec3f> mNormalScratch;

        /// An overall normal spread across a drawable's vertices, for the geometry that binds one.
        std::vector<osg::Vec3f> mFlatNormalScratch;
        std::vector<osg::Vec2f> mTexCoordScratch;
        std::vector<float> mMaskScratch;

        /// One terrain material's layers, refilled per chunk: a run is allocated by length and the
        /// length is only known once the passes with no texture on them have been passed over.
        std::vector<MaterialLayer> mLayerScratch;

        /// One emitter's sprites, refilled per particle system: the count is only known once the
        /// dead ones have been passed over, and a cell holds tens of these every frame.
        std::vector<Sprite> mSpriteScratch;

        /// The emitters this walk has met so far. Kept across walks and refilled, because this is
        /// the frame path and a cell holds tens of them.
        std::vector<PendingEmitter> mPending;
    };
}
