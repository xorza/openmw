#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/vfs/pathutil.hpp>

#include "deformertable.hpp"
#include "index.hpp"
#include "material.hpp"
#include "materialtable.hpp"
#include "meshinstance.hpp"
#include "meshrange.hpp"
#include "meshtable.hpp"
#include "placementtable.hpp"
#include "shaders/skinning.h"
#include "shapefold.hpp"
#include "spanallocator.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    /// One point light, placed in the world.
    ///
    /// Everything here is derived rather than read. A `LIGH` record carries a colour and a radius
    /// and **no intensity at all** — the original renderer had a fixed attenuation curve and no
    /// physical units, so brightness fell out of the curve and there is no authored value to be
    /// faithful to.
    struct Light
    {
        osg::Vec3f mPosition;

        /// Radiant intensity, linear, with the colour folded in.
        ///
        /// Scaled by the square of the recorded radius, which is what makes a lantern and a candle
        /// differ by their size rather than by an arbitrary per-light number.
        osg::Vec3f mIntensity;

        /// How far the light reaches, beyond which it contributes exactly nothing.
        ///
        /// **Not the recorded radius.** Morrowind's radii run 64 to 256 units in an interior — a
        /// metre to three and a half — because a fixed falloff curve lit the lamp's own post and an
        /// ambient term filled the room. Here the ambient is real light and the lamps have to be
        /// what lights the place, so the reach is stretched while the brightness is not.
        float mReach = 0.0f;

        /// How big the glowing part is, in world units.
        ///
        /// **Not the recorded radius**, and for once not a stretched version of it: this is the
        /// flame rather than the room it lights. `makeLight` is the one place it is derived and
        /// says what from; zero, which is what a light built by hand carries, is a point.
        ///
        /// A shadow ray opens to it, so a lamp with one casts a penumbra as wide as it is. It is
        /// also what stops the falloff running away at the lamp itself, which is where the air
        /// beside one is sampled — `falloff` says what that drew before.
        float mSourceRadius = 0.0f;

        /// How far short of the centre that ray stops.
        ///
        /// **A clearance, and it is not the same question as the size.** A lamp sits inside its own
        /// fitting — a lantern's frame, a sconce's bracket, a candle's holder — so a ray that runs
        /// all the way to the light ends among that fitting and comes back as fully shadowed. What
        /// `makeLight` estimates the fitting to be is the wider of the two.
        float mClearance = 0.0f;
    };

    /// One live particle, drawn as a disc facing the eye.
    ///
    /// **A particle system carries no triangles at all** — the sprites are the whole of the drawing —
    /// so nothing here reaches an acceleration structure. The layer is marched against the primary
    /// ray and composited instead, which is also what lets it blend in depth order without the
    /// candidate loop an alpha-blended hit would cost traversal.
    struct Sprite
    {
        osg::Vec3f mPosition;

        /// Half the sprite's width in world units, which is what `osgParticle` means by a size: its
        /// quad runs from `-size` to `+size` about the particle and its bounds are expanded by it.
        float mRadius = 0.0f;

        /// The streak's own axis in the world, per unit of `mRadius` — or **zero for a sprite that
        /// faces the eye**, which is nearly every one. `SpriteEmitter::mWidth` is the other half of
        /// the shape and is the emitter's, because a rotation cannot change it.
        ///
        /// **Per particle, because the rotation is.** `osgParticle` turns both of a quad's axes by
        /// the angle the particle carries before it draws them, and `Weather::RainShooter` is what
        /// leans a raindrop into the wind with it — so two drops fired under different winds hang at
        /// different angles in one frame, and an axis held once for the emitter drew the whole storm
        /// falling straight down.
        ///
        /// **Not normalised**, because its length is the shape: rain's is a whole radius against a
        /// width of a tenth, which is what makes a drop a streak.
        osg::Vec3f mAxis;

        /// Linear, and already carrying wherever the particle's own colour ramp has reached.
        osg::Vec3f mColour{ 1.0f, 1.0f, 1.0f };

        /// What the particle's own fade left of it, multiplied into the texture's alpha at the hit.
        float mAlpha = 1.0f;

        /// Where the particle stood on the previous frame, less where it stands now — see
        /// `Shaders::GpuSprite::mMoved` for why it is the difference that is carried.
        ///
        /// **The particle's own answer.** `osgParticle` keeps a previous position per particle for
        /// its own line rendering, so nothing here has to track a particle across frames or care
        /// that births and deaths reshuffle the array.
        osg::Vec3f mMoved;
    };

    /// One particle system: what its sprites are drawn with, and a sphere that holds all of them.
    ///
    /// **The sphere is the whole spatial structure and it is enough.** A light is asked for by a
    /// shading *point*, which the uniform grid answers in a lookup; an emitter is asked for by a
    /// whole *ray*, which would have to walk that grid cell by cell. There are tens of emitters in a
    /// cell against hundreds of lamps and each is small, so one rejection throws an emitter away for
    /// almost every pixel of the frame.
    struct SpriteEmitter
    {
        osg::Vec3f mCentre;

        /// Far enough from `mCentre` to contain every sprite in the range, rim included.
        float mReach = 0.0f;

        Index mFirst = 0;
        Index mCount = 0;

        /// The sprite texture, or `sNoIndex` where the emitter had none — which draws nothing, since
        /// a particle's whole silhouette is in that texture's alpha.
        Index mTexture = sNoIndex;

        /// What that texture's alpha leaves of the light crossing a sprite — a `SpriteLightMap` —
        /// or `sNoIndex` for one lit as a flat card.
        Index mLighting = sNoIndex;

        /// `SRC_ALPHA, ONE`: a flame, which adds light and hides nothing behind it. The rest blend
        /// over, which is smoke and needs its colour ramp to fade it.
        bool mAdditive = false;

        /// How wide this emitter's quads are against their own axis, per unit of `Sprite::mRadius`
        /// — or **nought for sprites that face the eye**, which is nearly every emitter in the game.
        ///
        /// `osgParticle` draws a particle as `position ± axisX * size ± axisY * size` and offers two
        /// ways of choosing those axes. A `BILLBOARD` system's are the screen's, transformed into
        /// view space every frame — that is a disc facing the eye and needs nothing carried here. A
        /// `FIXED` one's are used as they were authored, so the quad hangs in the world at an
        /// orientation of its own, and Morrowind's rain is the reason the mode exists: an X axis
        /// squashed to a tenth against a Y axis pointing straight down is a falling streak rather
        /// than a round drop.
        ///
        /// **The length of that X axis and not its direction**, because the march swings the width
        /// about the sprite's own axis to meet the ray rather than committing it to the plane the
        /// content picked. `Sprite::mAxis` carries the rest of the shape, and carries it per
        /// particle because a particle's own rotation turns it.
        float mWidth = 0.0f;
    };

    /// Everything the renderer needs to know about a world, with no Vulkan and no scene graph in it.
    ///
    /// Lights come from ESM `Light` records rather than from the graph: `NifOsg` never reads
    /// `NiLight`, so a model carries none — a candle's mesh and the light it casts arrive by
    /// different routes and are placed by the same reference.
    ///
    /// Deliberately dumb: it appends and it dedups paths, and nothing else. Deciding that two
    /// drawables are the same mesh belongs to whoever is reading the scene graph, which knows what
    /// identity means there; this type would have to guess.
    class SceneDesc
    {
    public:
        /// What a mesh's geometry may not straddle — `MeshTable::sVertexBlock` says why.
        static constexpr Index sVertexBlock = MeshTable::sVertexBlock;
        static constexpr Index sIndexBlock = MeshTable::sIndexBlock;

        /// Copies the vertex data into the shared buffers and returns the new mesh's index.
        ///
        /// `normals` and `texCoords` may be empty; when they are not they must match `positions` in
        /// length, and `indices` must be a whole number of triangles addressing only those vertices.
        /// All three are contracts on the caller, so they are asserted rather than reported.
        ///
        /// Throws where the mesh is longer than a block. **Named rather than asserted**, because a
        /// vertex count comes out of a content file and a run that straddled a block would be
        /// written across two device allocations that are not next to each other.
        ///
        /// `shape` is `MeshRange::mShape` and `deform` is `MeshRange::mDeform`, and both are the
        /// caller's findings: the scene keeps them and draws no conclusion of its own from the
        /// triangles it was handed. A mesh that deforms names the rig or the morph that poses it,
        /// whose vertex count must be this mesh's, and hands over its **bind pose**: the vertices a
        /// pose is computed from, which stay in the shared buffers for as long as the mesh does.
        /// `material` is `MeshRange::mMaterial`, the caller's finding likewise, and must be a
        /// material the scene holds or `sNoIndex`.
        Index addMesh(std::span<const osg::Vec3f> positions, std::span<const osg::Vec3f> normals,
            std::span<const osg::Vec2f> texCoords, std::span<const std::uint32_t> indices, FoldedShape shape = {},
            Deform deform = Deform::None, Index deformer = sNoIndex, Index material = sNoIndex);

        /// Copies a skin's runs and influences into the shared tables and returns the rig's index.
        ///
        /// `runs` is one word per vertex, `Shaders::RUN_COUNT_BITS` says how it is packed, and every
        /// run it names must lie inside `influences`; every `GpuInfluence::mBone` must be under
        /// `boneCount`. Contracts on the caller, asserted.
        Index addRig(
            std::span<const std::uint32_t> runs, std::span<const Shaders::GpuInfluence> influences, Index boneCount);

        /// Copies a morph's offsets — `targets` targets of `offsets.size() / targets` vertices each,
        /// laid end to end — into the shared table and returns the morph's index. `targets` must be
        /// at least one and divide `offsets.size()`.
        Index addMorph(std::span<const osg::Vec3f> offsets, Index targets);

        /// Poses one skinned mesh: its bone rows, and the box the pose reaches.
        ///
        /// **What a skinned body is, and the whole of what the host says about one per frame.** Its
        /// triangles never change and its vertices are computed on the device from the bind pose it
        /// arrived with, so what a frame hands over is one row per bone — a few dozen — and the
        /// mesh keeps the slot in the shared buffers every instance already names.
        ///
        /// `bones.size()` must be the rig's `mBoneCount`, a contract on the caller. `bounds` is the
        /// box the pose reaches in the mesh's own space, which the caller reads off the drawable
        /// rather than this walking vertices it does not have.
        ///
        /// The mesh joins `getDeformed` for the frame, which is what tells a backend whose vertices
        /// to pose and whose structure to refit — unless nothing moved: rows equal to the ones
        /// already held write nothing and name nothing, so an actor standing still two cells away
        /// costs no dispatch and no refit. Compared rather than trusted, because the walk poses
        /// every rig it meets and cannot know which of them the engine animated.
        void poseRig(Index mesh, std::span<const Shaders::GpuBone> bones, const osg::BoundingBoxf& bounds);

        /// The same for a morphed mesh: one weight per target of its morph, the base's included and
        /// ignored, which is how `SceneUtil::MorphGeometry` numbers them.
        void poseMorph(Index mesh, std::span<const float> weights, const osg::BoundingBoxf& bounds);

        Index addMaterial(const Material& material);

        /// Rewrites a material in place, keeping its slot and everything standing on it.
        ///
        /// **For shading that animates rather than for a mistake.** A `NifOsg` flipbook, UV, alpha
        /// or material-colour controller rewrites its state set every frame and the surface wearing
        /// it is the same surface: adding a material and dropping the old one would churn a slot a
        /// frame and leave every placement pointing at it to be found and repointed.
        ///
        /// Writing back what is already there costs nothing — the row is only reported as written
        /// when something actually changed, so a paused game re-reads its fires and uploads none
        /// of them.
        ///
        /// **A material that changes what traversal is told rewrites every placement wearing it.**
        /// Its kind, whether it is a cutout and whether it is translucent go into the acceleration
        /// structure's row and not only into the material table; an alpha controller fading a
        /// surface past opaque changes those rows, so the slots wearing the material join
        /// `getMoved`. A flipbook turning or a texture scrolling changes none of them and costs the
        /// placements nothing.
        void setMaterial(Index material, const Material& what);

        /// Copies `weights` into the shared mask table and returns where they landed.
        ///
        /// One float per weight rather than the byte the source holds: a mask is a few hundred
        /// texels and a whole cell's worth is tens of kilobytes, which is not worth requiring
        /// 8-bit storage of the device for.
        ///
        /// How long the run is is not stored beside the offset: a mask is a grid, and the layer
        /// that names this carries the two sides of it. `release` reconstructs the length from
        /// them, so a caller whose weights are not `mMaskWidth * mMaskHeight` long leaks the
        /// difference.
        Index addMask(std::span<const float> weights);

        /// Copies a material's layers into the shared layer table and returns where they landed.
        ///
        /// **All of them at once, because a run is allocated as a run.** They were appended one at
        /// a time when the table only ever grew and a material took whatever length the table
        /// happened to be at; a run that can be given back has to be asked for by length.
        Span addLayers(std::span<const MaterialLayer> layers);

        void addLight(const Light& light);

        /// Returns the slot of an image this renderer made, adding it only if `key` is not known.
        ///
        /// **A texture with no file behind it, which the table has to be able to hold.** A composite
        /// baked for a distant terrain chunk is an image nothing can open: the bytes belong to
        /// whatever made it, and what the scene keeps is the slot, because a slot is what a material
        /// points at and what a backend uploads into. Two chunks that would bake the same image must
        /// find the same slot, which is what `key` is for and why it has to be stable across frames.
        ///
        /// The same slots, the same free list and the same reference counting as a file's — this is a
        /// second way in and not a second table. `holdTexture` and `dropTexture` do not care which
        /// kind a slot is.
        Index addBakedTexture(std::string_view key);

        /// Returns the index of `path`, adding it only if it is not already known.
        ///
        /// **The slot is live from here**, before anything names it, and stays live until the last
        /// thing that named it lets go. A caller that adds a texture and then puts it on no material
        /// and takes no hold of it keeps that slot for the rest of the scene, which is a caller
        /// asking for a texture it did not want.
        Index addTexture(VFS::Path::NormalizedView path);

        /// Names a texture for something no material can speak for, and stops.
        ///
        /// **A particle emitter's sprite, and nothing else so far.** An emitter is a placement — it
        /// is thrown away and rebuilt every frame — so the texture it draws with hangs off no
        /// material and no table the scene owns; whatever recognises the emitter between frames is
        /// what has to hold it. The alternative was a keep set handed over on every sweep, which
        /// could only be looked at on the frames a mesh or a material also died.
        ///
        /// `sNoIndex` is allowed and does nothing, so a caller need not test what it got.
        void holdTexture(Index texture);

        /// Gives back one `holdTexture`. The slot is freed here where nothing else names it.
        void dropTexture(Index texture);

        /// Whether nothing stands in `slot`: the last thing naming it gave it back, and it is
        /// waiting for the next `addTexture` to take it over.
        ///
        /// **The name and not the reference count**, which is the same answer except for the window
        /// between a slot being handed out and whatever is about to name it doing so. A reader that
        /// asked the count would find a texture it was in the middle of building.
        bool isTextureFree(Index texture) const { return mTextures.isFree(texture); }

        /// Places `instance` in a slot and returns it.
        ///
        /// **The slot is the placement's name for as long as it stands.** It is the custom index a
        /// hit reads back, the row a shader looks its material up in, and — because it outlives the
        /// walk that made it — what lets a mirror move a placement instead of rebuilding the list
        /// it was in. A slot freed by `dropInstance` is handed out again; one that is still standing
        /// never is.
        Index addInstance(const MeshInstance& instance);

        /// Moves the placement in `slot`, and says whether that changed anything.
        ///
        /// A transform equal to the one already there is not a move: it writes nothing, records
        /// nothing, and leaves the slot reporting no motion. That is the ordinary case — most of a
        /// world stands still — and making it the cheap one is the point of addressing placements
        /// by slot at all.
        bool moveInstance(Index slot, const osg::Matrixf& transform);

        /// Fades the placement in `slot`.
        ///
        /// Separate from `moveInstance` because the two are separate facts: an actor fading on the
        /// spot has not moved, and an actor walking is not fading. A fade that changed the number
        /// joins `getMoved` all the same, because it is a row to rewrite — the opacity a shader
        /// reads and the translucency traversal is told — and its previous transform stays equal to
        /// its current one, so it carries no motion.
        void fadeInstance(Index slot, float opacity);

        /// Empties `slot`. Its index is not reused until the next `addInstance` asks for one.
        ///
        /// The slot joins `getMoved`: a backend has to write its row inactive, or the structure
        /// goes on tracing what stood there.
        void dropInstance(Index slot);

        /// Ends a frame's placement: what moved becomes where things were.
        ///
        /// **Costs what moved and not what stands.** Only a slot that reported a move can have a
        /// previous transform that differs from its current one, so only those have to be caught
        /// up — which is what makes a world of fifty thousand placements and three hundred movers
        /// cost three hundred. What was moved becomes `getSettled`, and `getMoved` starts empty.
        void advancePlacement();

        /// Appends one particle system's live sprites, and the emitter that names them.
        ///
        /// The sphere is derived here rather than passed in, so the rejection test a ray makes and
        /// the sprites it would then walk cannot disagree about where they are. Nothing is added for
        /// an emitter with no live particles, which is most of them for most of a frame.
        /// @param width how wide the quads are against their own axis, per unit of
        ///        `Sprite::mRadius`, or nought for sprites that face the eye. Every sprite carries
        ///        an axis where this is set and none where it is not — `SpriteEmitter::mWidth`.
        /// @param lighting the bake of `texture`'s alpha, or `sNoIndex`. `SpriteEmitter::mLighting`.
        void addEmitter(std::span<const Sprite> sprites, Index texture, bool additive, float width = 0.0f,
            Index lighting = sNoIndex);

        /// Drops every mesh and material the caller did not name.
        ///
        /// **The only way a scene loses geometry, and nothing is renumbered by it.** A freed entry
        /// keeps its index and its room; the index goes on a free list and the next arrival that
        /// fits takes the slot. Compacting instead — closing the gaps and renaming what pointed into
        /// them — is what made a cell boundary cost a full rebuild: every bottom-level acceleration
        /// structure in the world is named by a mesh index, and every material a hit reads is named
        /// by another.
        ///
        /// **Textures are not swept here and are not named here.** A material freed below gives back
        /// what it named on its way out, which is the same thing `setMaterial` does when a shading
        /// animation stops naming an image and the same thing `dropTexture` does for an emitter's
        /// sprite. Sweeping them instead meant asking on the frames a mesh or a material happened to
        /// die as well, and a texture that stopped being named on any other frame was never noticed.
        ///
        /// Layers and masks have no keep set either: they belong to the material that owns them, so
        /// a freed material hands both runs back to their allocators on its way out. A terrain
        /// chunk's masks are tens of kilobytes and a player can cross the whole continent through
        /// one `SceneDesc`, so leaving them to the sweep that eventually drops the material was a
        /// session-long growth.
        ///
        /// **Placements do not go**, and they no longer have to be carried anywhere either: a slot
        /// is a name, and what it names has stopped moving.
        ///
        /// @param meshes every mesh to keep, each once, in any order.
        /// @param materials the same for materials.
        /// @return whether anything was freed. False is the ordinary frame, and it costs two
        ///         comparisons: a scene that lost nothing has as many survivors as it had entries.
        bool release(std::span<const Index> meshes, std::span<const Index> materials);

        /// Empties the per-frame lists a walk rebuilds wholesale: lights, deformed meshes, sprites
        /// and emitters.
        ///
        /// **Placements are not among them.** They are addressed by slot and reconciled in place —
        /// `addInstance` for one that has appeared, `moveInstance` for one that has shifted,
        /// `dropInstance` for one that has gone — because a slot index is what a hit reads back and
        /// because a world of fifty thousand placements of which three hundred move should cost
        /// three hundred. Everything above is small enough per frame that rebuilding it is cheaper
        /// than reconciling it.
        void clearPlacement();

        /// **Every span below is into a table that grows, and lives until the table does.** A span
        /// is valid until the next `add` into its table — `addMesh` grows the geometry and the mesh
        /// table, `addEmitter` the sprites and the emitters, and each of the others the table it
        /// names — and until `clearPlacement`, which empties the per-frame ones. Take it after the
        /// add and never in the same expression as one: `getMeshes()[addMesh(...)]` sequences the span
        /// before the add, and indexes a table that has moved.
        ///
        /// **A row read out of one by reference has the same lifetime as the span it came from**,
        /// which is what makes an index the only name for a row that outlives an add. A caller
        /// holding the scene `const` is safe by its type. A caller that builds one is not, and
        /// finishing the adds before the reads is the shape that does not have to remember it.
        std::span<const osg::Vec3f> getPositions() const { return mMeshTable.getPositions(); }
        std::span<const osg::Vec3f> getNormals() const { return mMeshTable.getNormals(); }
        std::span<const osg::Vec2f> getTexCoords() const { return mMeshTable.getTexCoords(); }
        std::span<const std::uint32_t> getIndices() const { return mMeshTable.getIndices(); }

        /// Every mesh slot, live or free. A freed one has a zero count and keeps its room, so a
        /// backend that walks these builds a structure over nothing rather than over somebody else's
        /// triangles — and the top level a frame rebuilds is what stops it being traced.
        std::span<const MeshRange> getMeshes() const { return mMeshTable.getRows(); }

        /// Which meshes changed shape since the last `clearPlacement`, each named once and in no
        /// particular order. Empty for a world that only moves.
        std::span<const Index> getDeformed() const { return mMeshTable.getDeformed(); }

        /// Every rig slot, live or free — `Rig::mUses` tells them apart — and the two tables the rigs
        /// index.
        std::span<const Rig> getRigs() const { return mDeformers.getRigs(); }
        std::span<const std::uint32_t> getRuns() const { return mDeformers.getRuns(); }
        std::span<const Shaders::GpuInfluence> getInfluences() const { return mDeformers.getInfluences(); }

        /// The same for the morphs.
        std::span<const Morph> getMorphs() const { return mDeformers.getMorphs(); }
        std::span<const osg::Vec3f> getMorphOffsets() const { return mDeformers.getMorphOffsets(); }

        /// Every deforming mesh's pose, laid end to end: a run of rows per skinned mesh, and a run
        /// of weights per morphed one. `MeshRange::mPoseOffset` says where each starts.
        std::span<const Shaders::GpuBone> getBones() const { return mDeformers.getBones(); }
        std::span<const float> getWeights() const { return mDeformers.getWeights(); }

        /// One mesh's pose, for a backend writing that mesh's rows or a test reading them back.
        std::span<const Shaders::GpuBone> getMeshBones(Index mesh) const;
        std::span<const float> getMeshWeights(Index mesh) const;

        /// How many vertices the deforming meshes' bind poses take between them, which is how long
        /// a backend's bind table has to be. `MeshRange::mBindOffset` says where each mesh's run is.
        Index getBindVertexCount() const { return mDeformers.getBindVertexCount(); }

        /// Which rig and morph slots have been written since the last `clearArrivals`, for a
        /// backend to upload. A freed slot is named by nothing: nothing reads it until the next
        /// arrival lands in it, and that arrival names it.
        std::span<const Index> getArrivedRigs() const { return mDeformers.getArrivedRigs(); }
        std::span<const Index> getArrivedMorphs() const { return mDeformers.getArrivedMorphs(); }
        /// Every slot, standing or empty, in slot order. `MeshInstance::isPlaced` tells them apart.
        std::span<const MeshInstance> getInstances() const { return mPlacements.getAll(); }

        /// How many slots hold a placement, which is what reaches an acceleration structure.
        std::uint32_t getPlacedCount() const { return mPlacements.getPlacedCount(); }

        /// Where each slot stood before the last `advancePlacement`, indexed alongside the slots.
        std::span<const osg::Matrixf> getPrevious() const { return mPlacements.getPrevious(); }

        /// The slots whose row changed since the last `advancePlacement`: placed, moved, faded,
        /// dropped, or wearing a material that changed what traversal is told.
        ///
        /// **What a backend rewrites, and all it rewrites.** A world is tens of thousands of
        /// placements and a frame changes hundreds; a row table written whole every frame was a
        /// millisecond of the game's CPU to change nothing. A slot can appear more than once where
        /// two facts about it changed in one frame, which costs one row written twice.
        std::span<const Index> getMoved() const { return mPlacements.getMoved(); }

        /// The slots the last `advancePlacement` caught up, whose motion is now still.
        ///
        /// **The other half of what a backend rewrites.** A row carries the motion between where a
        /// placement stood and where it stands, and that motion goes back to nothing on the frame
        /// after the move — which is a frame on which the slot did not move. Without this list a
        /// backend writing only `getMoved` would leave last frame's motion in the row for ever.
        std::span<const Index> getSettled() const { return mPlacements.getSettled(); }
        std::span<const Material> getMaterials() const { return mMaterialTable.getRows(); }
        std::span<const MaterialLayer> getLayers() const { return mMaterialTable.getLayers(); }
        std::span<const Light> getLights() const { return mLights; }

        /// Puts the lights in an order that depends on the lights and not on the walk that found
        /// them. Once, where a walk ends.
        ///
        /// **A picture must not depend on the order cells were loaded in.** A walk meets lights in
        /// graph order, and a graph gains and loses cells as a player moves — so the same place
        /// walked twice hands the same lights over in a different order. Every one of them is still
        /// there, and the picture still changes: the grid bins them in that order and a reservoir
        /// streams them in it, so one cell's sample falls on a different lamp and its neighbourhood
        /// moves by a level or two. It is a handful of pixels around one lamp, which is small enough
        /// to be read as noise and is not.
        void orderLights();

        /// How many times the scene's **structure** has changed: its meshes and its textures.
        ///
        /// **What a rebuild costs is why only these two are counted.** A mesh appearing means a
        /// bottom-level acceleration structure that does not exist yet, and a texture appearing
        /// means an array that has to be made again — hundreds of milliseconds between them, and
        /// the temporal history goes with them. Nothing else in the scene is worth that.
        ///
        /// **The only honest test for it**: comparing table sizes misses a cell that left as another
        /// arrived, which is exactly what walking across a boundary does — and it misses a freed
        /// slot taken over by something else entirely, which is what one does now. Bumped by a mesh
        /// or a texture appearing, whether at the end of the table or into a slot something else
        /// left; never by a placement, which is rewritten every frame anyway.
        std::uint64_t getStructureRevision() const { return mMeshTable.getRevision() + mTextures.getRevision(); }

        /// Forgets what has arrived and what has gone, for a caller that has applied both.
        ///
        /// **Whoever hands the scene to a backend owns this**, not the frame: an arrival lives from
        /// the walk that made it until something has taken it, and a walk that is never handed over
        /// must not lose what it added.
        void clearArrivals();

        /// Which texture slots have been written since the last `clearArrivals`.
        ///
        /// **A list and not a count, because a slot is taken over wherever it sits.** A backend used
        /// to be handed the tail of the table and told to append; reclaiming a slot means an arrival
        /// can be anywhere, so the arrivals say where each one goes and the backend writes those and
        /// nothing else.
        std::span<const Index> getArrivedTextures() const { return mTextures.getArrived(); }

        /// Which mesh slots have been written since the last `clearArrivals`.
        ///
        /// The same list for the expensive half. `getMeshRevision` says *that* a mesh arrived and a
        /// backend hearing it had nothing to do but build the scene again; this says *which*, which
        /// is what lets it build those structures and leave the rest standing.
        std::span<const Index> getArrivedMeshes() const { return mMeshTable.getArrived(); }

        /// Which mesh slots `release` has given up since the last `clearArrivals`.
        std::span<const Index> getFreedMeshes() const { return mMeshTable.getFreed(); }

        /// Which texture slots `release` has given up since the last `clearArrivals`.
        ///
        /// **What lets a backend stop holding a departed cell's images.** An array that is never
        /// told a slot went keeps whatever was in it until something takes the slot over, so a
        /// region walked away from goes on costing its texture memory.
        std::span<const Index> getFreedTextures() const { return mTextures.getFreed(); }

        /// How many times a **mesh** has appeared, which is the expensive half of the above.
        ///
        /// A texture arriving is an upload; a mesh arriving is a bottom-level acceleration structure
        /// that does not exist yet. Told apart because a body texture nobody has worn yet must not
        /// cost the structures of a whole cell.
        std::uint64_t getMeshRevision() const { return mMeshTable.getRevision(); }

        /// Which material slots `addMaterial` or `setMaterial` wrote since the last `clearArrivals`,
        /// each once.
        ///
        /// **Rows and not a revision, because a row is what a backend writes.** A counter that moved
        /// on any material said "the shading changed" and the answer to that was every material,
        /// every layer and every mask copied to the device — megabytes, every frame a flipbook
        /// turned, to change eighty bytes. A material the sweep freed is not here: nothing stands on
        /// it, so its row is never read and need not be written.
        std::span<const Index> getWrittenMaterials() const { return mMaterialTable.getWritten(); }

        /// The runs `addLayers` placed since the last `clearArrivals`, and the same for `addMask`.
        ///
        /// **Runs and not a flag over the table**, for the reason the materials are rows: a chunk
        /// arriving writes its own layers and its own weights, and the rest of both tables is what
        /// it was. A run the sweep gave back is not named here either — nothing reads it until the
        /// next chunk lands in it, and that chunk's arrival is what names it.
        std::span<const Span> getArrivedLayers() const { return mMaterialTable.getArrivedLayers(); }
        std::span<const Span> getArrivedMasks() const { return mMaterialTable.getArrivedMasks(); }

        std::span<const Sprite> getSprites() const { return mSprites; }
        std::span<const SpriteEmitter> getEmitters() const { return mEmitters; }
        std::span<const float> getMasks() const { return mMaterialTable.getMasks(); }
        /// The file each slot was read from, empty where it was not read from one.
        std::span<const VFS::Path::Normalized> getTextures() const { return mTextures.getPaths(); }

        /// What made each slot, for the ones nothing opened — empty for every slot that is a file.
        ///
        /// **Parallel to `getTextures` and not instead of it**, because the two are different facts
        /// about a slot and nearly every reader wants only the first. A slot with neither is one
        /// nothing stands in, which is what `isTextureFree` answers.
        std::span<const std::string> getBakedTextures() const { return mTextures.getBaked(); }

        /// The vertices of one mesh, for a test or a build that wants to read back what it appended.
        std::span<const osg::Vec3f> getMeshPositions(Index mesh) const;
        std::span<const std::uint32_t> getMeshIndices(Index mesh) const;

        std::uint32_t getTriangleCount() const;

        /// The world-space extent of everything placed. Invalid when nothing is.
        ///
        /// Computed from each mesh's local box carried through its instances rather than from every
        /// vertex of every instance, which is the difference between eight transforms per instance
        /// and several hundred.
        ///
        /// **Backdrops included, and a far plane is what wants that**: a ray has to reach the sea,
        /// so what the frame must span is everything there is. `getContentBounds` is the other
        /// question.
        osg::BoundingBoxf getBounds() const;

        /// The extent of what stands inside `region`, backdrops left out. Invalid where nothing does.
        ///
        /// **What a camera is placed from, and neither half of it is optional.** The sea is one sheet
        /// a hundred and fifty cells across and the ground now reaches four cells past the one being
        /// looked at, so a camera framing the whole scene went a million and a half units out and
        /// photographed water — and framing everything that is not the sea would still go two hundred
        /// thousand out and photograph a region. A view of a place is a view of that place.
        ///
        /// `region` is asked in world units and clips what it meets, so a chunk straddling its edge
        /// contributes the part inside it rather than dragging the answer a cell wide. Its height is
        /// the caller's to leave open: how high the ground is there is exactly what this is for.
        ///
        /// Water is the only backdrop today and `MaterialKind` is what says so; a sky dome would join
        /// it here rather than teaching every caller a second exception.
        osg::BoundingBoxf getContentBoundsWithin(const osg::BoundingBoxf& region) const;

        /// Bytes held by the vertex and index buffers. What the upload at M3 will cost.
        std::size_t getGeometryBytes() const;

    private:
        /// What poses the deforming meshes, and the poses themselves. Its own type, for the reason
        /// the tables below are: a rig, the meshes counted on it and the runs behind both are one
        /// invariant.
        ///
        /// **Before the meshes, which borrow it.** A mesh stands its deformer as it arrives and
        /// releases one as it goes, and the count is this table's.
        DeformerTable mDeformers;

        /// Every mesh, and the shared buffers its triangles live in.
        MeshTable mMeshTable{ mDeformers };

        /// Where everything stands and which rows a backend has to write again. Its own type,
        /// because slots that are never moved, a free list and two change lists are one invariant.
        PlacementTable mPlacements;

        std::vector<Light> mLights;
        std::vector<Sprite> mSprites;
        std::vector<SpriteEmitter> mEmitters;

        /// Every texture the scene names, and what still names each. Its own type, because a
        /// reference-counted table with a free list and two lookups into it is a thing with an
        /// invariant rather than a set of parallel vectors.
        ///
        /// **Before the materials, which borrow it.** A material names texture slots and gives them
        /// back as it is rewritten and as it is swept, and the count is this table's.
        TextureTable mTextures;

        /// The materials, their terrain layers and the weights those place. Its own type, for the
        /// reason the two above are: a row, the runs it names and what those name are one
        /// invariant rather than ten members held in step by hand.
        MaterialTable mMaterialTable{ mTextures };

        /// Calls `visit(instance, worldBox)` for every placement, which is what both extents walk.
        template <class Visit>
        void forEachPlacement(Visit&& visit) const;
    };
}
