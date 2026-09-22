#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <osg/BoundingBox>

#include "deformertable.hpp"
#include "lightbuilder.hpp"
#include "material.hpp"
#include "materialtable.hpp"
#include "mesh.hpp"
#include "meshtable.hpp"
#include "placementtable.hpp"
#include "refusals.hpp"
#include "result.hpp"
#include "ripple.hpp"
#include "runs.hpp"
#include "shaders/skinning.h"
#include "shapefold.hpp"
#include "sprite.hpp"
#include "stepped.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    /// What `SceneDesc::addMesh` answers for a mesh that brought its own deformer: both rows, the
    /// deformer's for the meshes that will share it.
    struct DeformedMesh
    {
        Index mMesh = sNoIndex;
        Index mDeformer = sNoIndex;
    };

    /// Everything the renderer needs to know about a world, with no Vulkan and no scene graph in
    /// it, and what of the world it could not take. It appends and it dedups paths, and nothing
    /// else: deciding that two drawables are the same mesh belongs to whoever is reading the scene
    /// graph.
    class SceneDesc
    {
    public:
        /// What a mesh's geometry may not straddle — `MeshTable::sVertexBlock` says why.
        static constexpr Index sVertexBlock = MeshTable::sVertexBlock;
        static constexpr Index sIndexBlock = MeshTable::sIndexBlock;

        SceneDesc();

        /// Moved whole: no table holds a reference to a sibling, so what a moved description's
        /// tables reach is its own. Every fact that crosses two tables is asked of this class,
        /// which hands its own members over — `addMaterial`, `addMesh`, `setMaterial`, `release`.
        SceneDesc(SceneDesc&&) noexcept = default;
        SceneDesc& operator=(SceneDesc&&) noexcept = default;

        /// Never copied: a copy would be two descriptions under one identity, and a backend built
        /// from either would append the other's arrivals onto its own tables.
        SceneDesc(const SceneDesc&) = delete;
        SceneDesc& operator=(const SceneDesc&) = delete;

        /// Which description this is, for a backend that holds a slot built from one: never
        /// nought, never handed out twice in a process, and kept across a move — so a slot can say
        /// what it was built from without naming an address a later description may take over.
        std::uint64_t getIdentity() const { return mIdentity; }

        /// Copies the vertex data into the shared buffers and returns the new mesh's index. Every
        /// attribute but `MeshArrays::mPositions` may be empty; when one is not it must match the
        /// positions in length, `MeshArrays::mIndices` must be a whole number of triangles
        /// addressing only those vertices, and the mesh must fit a block (`MeshTable::checkFits`)
        /// — asserted, as a contract on the caller, which reads the counts out of a content file
        /// and refuses what does not hold. A mesh that deforms names the rig or the morph that
        /// poses it, whose vertex count must be this mesh's, and hands over its bind pose, which
        /// stays in the shared buffers for as long as the mesh does.
        Index addMesh(
            const MeshArrays& arrays, FoldedShape shape = {}, Deform deform = Deform::None, Index deformer = sNoIndex);

        /// `addMesh` for a mesh that brings the skin or the targets that pose it. The deformer row
        /// and the mesh row are made in one call with nothing between them, so a deformer no mesh
        /// stands on cannot exist — a row nothing would free, because a deformer goes with its
        /// last mesh. The spec poses exactly this mesh's vertices (`checkPoses`) and the mesh fits a
        /// block, both asserted. A second mesh on the same skin names the deformer answered here
        /// through the overload above.
        DeformedMesh addMesh(const MeshArrays& arrays, FoldedShape shape, const RigSpec& rig);
        DeformedMesh addMesh(const MeshArrays& arrays, FoldedShape shape, const MorphSpec& morph);

        /// Whether a deformer of `posed` vertices can pose `arrays`, and why not where they are
        /// of another length. Both counts come out of a content file, so whoever reads them asks
        /// this before `addMesh`, which asserts it.
        static Result<void, std::string> checkPoses(Index posed, const MeshArrays& arrays);

        /// Poses one deforming mesh: its bone rows or its target weights as `packBones` or
        /// `packWeights` lays them, and the box the pose reaches — the whole of what the host says
        /// about a body per frame, because its vertices are computed on the device from the bind
        /// pose. The words must be as many as the deformer's pose takes. The mesh joins
        /// `getDeformed` for the frame unless nothing moved: words equal to the ones already held
        /// write nothing, so an actor standing still costs no dispatch and no refit. Compared rather
        /// than trusted, because the walk poses every rig it meets.
        void pose(Index mesh, std::span<const PoseWord> words, const osg::BoundingBoxf& bounds);

        /// Puts `material` in a slot of the material table, holding every texture it names, and
        /// returns it. The one way in: a material names textures, and what crosses two tables is
        /// this class's to do.
        Index addMaterial(const Material& material);

        /// Rewrites a material in place, keeping its slot and everything standing on it — for
        /// shading that animates: a `NifOsg` flipbook or alpha controller rewrites its state set
        /// every frame and the surface wearing it is the same surface. Writing back what is already
        /// there costs nothing. A material that changes what traversal is told — its kind, cutout,
        /// translucent — rewrites every placement wearing it, because those go into the
        /// acceleration structure's row; a flipbook turning costs the placements nothing.
        void setMaterial(Index material, const Material& what);

        void addLight(const Light& light);

        /// Whether a hold on a mesh or a material went to nought since the last `release`.
        bool hasDroppedHolds() const;

        /// Places `instance` in a slot and returns it. The slot is the placement's name for as long
        /// as it stands: the custom index a hit reads back, and what lets a mirror move a placement
        /// instead of rebuilding the list it was in. The mesh and the material it names must be
        /// live rows, which is asserted.
        Index addInstance(const MeshInstance& instance);

        /// Whether every placement names a live mesh and a live material: what `release` asserts
        /// after a sweep, because a placement standing on a freed row would be traced against
        /// whatever the slot is given next, and nothing else would say so.
        bool placementsStandOnLiveRows() const;

        /// Whether every live row of every table is held or named — what a sweep leaves true, and
        /// what a caller that took a slot and forgot to hold it breaks: no live texture and no
        /// live deformer with a hold count of nought, and every placement on live rows. Neither
        /// table has a sweep that could find such a row, so the extractor asks this at the end of
        /// every retire.
        bool isConsistent() const;

        /// Whether nothing stands: no live mesh, material, texture or deformer, and no placement.
        /// What a detached world is, and what a mirror asserts of its scene at teardown.
        bool isEmpty() const;

        /// Appends one particle system's live sprites, and the emitter that names them. The sphere
        /// is derived here rather than passed in, so the rejection test a ray makes and the sprites
        /// it would then walk cannot disagree about where they are. Nothing is added for an emitter
        /// with no live particles.
        /// @param width how wide the quads are against their own axis, per unit of
        ///        `Sprite::mRadius`, or nought for sprites that face the eye. Every sprite carries
        ///        an axis where this is set and none where it is not — `SpriteEmitter::mWidth`.
        /// @param lighting the bake of `texture`'s alpha, or `sNoIndex`. `SpriteEmitter::mLighting`.
        /// @param falls whether the sprites fall from the sky. `SpriteEmitter::mFalls`.
        void addEmitter(std::span<const Sprite> sprites, Index texture, bool additive, float width = 0.0f,
            Index lighting = sNoIndex, bool falls = false);

        /// Drops every mesh and material the caller did not name, each named once in any order —
        /// the only way a scene loses geometry, and nothing is renumbered by it: every bottom-level
        /// acceleration structure is named by a mesh index, and compacting is what made a cell
        /// boundary cost a full rebuild. Textures are not swept here: a material freed gives back
        /// what it named on its way out, as `setMaterial` and `TextureTable::drop` do, and its
        /// layer and mask runs go with it. Placements do not go: a slot is a name, and what it
        /// names has stopped moving. Answers whether anything was freed, and false is the ordinary
        /// frame at the cost of two comparisons: a scene that lost nothing has as many survivors as
        /// it had entries.
        bool release(std::span<const Index> meshes, std::span<const Index> materials);

        /// Empties the per-frame lists a walk rebuilds wholesale: lights, deformed meshes, sprites
        /// and emitters. Placements are not among them: they are reconciled in place through
        /// `placements()`, because a world of fifty thousand placements of which three hundred move
        /// should cost three hundred.
        void clearPlacement();

        /// The tables, for whoever builds the scene to write straight into and for whoever is
        /// handed it to read. Only what crosses tables stays on this class; everything that reads
        /// or writes one table is asked of that table. Every span a table hands out is valid until
        /// the next `add` into its table and until `clearPlacement`: take it after the add and never
        /// in the same expression as one.
        MeshTable& meshes() { return mMeshes; }
        MaterialTable& materials() { return mMaterials; }
        TextureTable& textures() { return mTextures; }
        PlacementTable& placements() { return mPlacements; }
        DeformerTable& deformers() { return mDeformers; }

        const MeshTable& meshes() const { return mMeshes; }
        const MaterialTable& materials() const { return mMaterials; }
        const TextureTable& textures() const { return mTextures; }
        const PlacementTable& placements() const { return mPlacements; }
        const DeformerTable& deformers() const { return mDeformers; }

        std::span<const Light> lights() const { return mLights; }
        std::span<const Sprite> sprites() const { return mSprites; }
        std::span<const SpriteEmitter> emitters() const { return mEmitters; }

        /// What of the content handed to this scene could not be used as it stands.
        Refusals& refusals() { return mRefusals; }
        const Refusals& refusals() const { return mRefusals; }

        /// What disturbed the water this frame, for the ripple field to press. A frame's list like
        /// the sprites', cleared with the placement: a wake is a fact about a frame and not about
        /// a cell.
        std::span<const RippleImpulse> ripples() const { return mRipples; }
        void addRipple(const RippleImpulse& impulse)
        {
            mTurn.expect(Turn::Open, Turn::Walked);
            mRipples.push_back(impulse);
        }

        /// What a backend compares against to know whether the geometry or the textures it built
        /// from are still the ones the scene holds.
        std::uint64_t getStructureRevision() const { return mMeshes.getRevision() + mTextures.getRevision(); }

        /// The pose a deforming mesh was last given, in words. Crosses two tables: the mesh row
        /// says where its run sits, and the deformers hold it.
        std::span<const PoseWord> getMeshPose(Index mesh) const;

        /// Every placement's own box, in the world.
        osg::BoundingBoxf getBounds() const;

        /// The same, clipped to `region` and with the water left out: the sea is one sheet a
        /// hundred and fifty cells across, so a caller asking how far the ground reaches would
        /// clear any threshold at every coastline.
        osg::BoundingBoxf getContentBoundsWithin(const osg::BoundingBoxf& region) const;

        /// Sorts the lights so that a frame's own order is a fact about the world. `SceneUploader`
        /// makes it at the one point every path passes, because a walk may run twice — which is
        /// also where the hand-over of a scene a walk left unswept is refused (`Turn`).
        void orderLights();

        /// Forgets what arrived and what was freed, which a hand-over does once it has read both.
        void clearArrivals();

        /// Says a walk is placing into this scene, and that the sweep after it has run: the
        /// extractor's two calls, at every walk and at every `retire`. What they keep is the turn
        /// below, which is what refuses a hand-over of a scene a walk left unswept.
        void noteWalked();
        void noteSwept();

    private:
        /// Where the frame stands: open to the walks that fill its lists, walked and owing the
        /// sweep after it, or handed to a backend that has read them.
        ///
        /// **A walk marks the scene until the sweep clears it, and a hand-over refuses the
        /// mark.** A walk stamps what it met and leaves what it did not standing; only the sweep
        /// takes that. Handed over between the two, the scene still holds every slot the walk
        /// stopped finding, where the last frame left it — a crate picked up traced once more, a
        /// body whose identity moved traced twice. The mark survives `clearPlacement`, because
        /// clearing the frame's lists settles nothing of the sweep.
        ///
        /// A light, an emitter or a ripple added after the hand-over and before the next
        /// `clearPlacement` is one the frame lost or the next frame doubled, and it is asserted
        /// where it is added.
        enum class Turn
        {
            Open,
            Walked,
            Handed,
        };

        template <class Visit>
        void forEachPlacement(Visit&& visit) const;

        std::uint64_t mIdentity;

        Stepped<Turn> mTurn{ Turn::Open };

        TextureTable mTextures;
        DeformerTable mDeformers;

        /// Every mesh, and the shared buffers its triangles live in.
        MeshTable mMeshes;

        /// The materials, their terrain layers and the weights those place.
        MaterialTable mMaterials;

        /// Where everything stands and which rows a backend has to write again.
        PlacementTable mPlacements;

        std::vector<Light> mLights;
        std::vector<Sprite> mSprites;
        std::vector<SpriteEmitter> mEmitters;
        std::vector<RippleImpulse> mRipples;

        Refusals mRefusals;
    };
}
