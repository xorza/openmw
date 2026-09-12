#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Matrixf>
#include <osg/Vec3f>

#include <components/vfs/pathutil.hpp>

#include "deformertable.hpp"
#include "lightbuilder.hpp"
#include "material.hpp"
#include "materialtable.hpp"
#include "mesh.hpp"
#include "meshtable.hpp"
#include "placementtable.hpp"
#include "runs.hpp"
#include "shaders/skinning.h"
#include "shapefold.hpp"
#include "sprite.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    class SceneDesc
    {
    public:
        /// What a mesh's geometry may not straddle — `MeshTable::sVertexBlock` says why.
        static constexpr Index sVertexBlock = MeshTable::sVertexBlock;
        static constexpr Index sIndexBlock = MeshTable::sIndexBlock;

        /// Copies the vertex data into the shared buffers and returns the new mesh's index.
        ///
        /// Every attribute but `MeshArrays::mPositions` may be empty; when one is not it must match
        /// the positions in length, and `MeshArrays::mIndices` must be a whole number of triangles
        /// addressing only those vertices — asserted, as a contract on the caller. Throws where the
        /// mesh is longer than a block, because a vertex count comes out of a content file.
        ///
        /// `shape`, `deform` and `material` are the caller's findings, kept as `MeshRange`'s. A
        /// mesh that deforms names the rig or the morph that poses it, whose vertex count must be
        /// this mesh's, and hands over its bind pose, which stays in the shared buffers for as long
        /// as the mesh does.
        Index addMesh(const MeshArrays& arrays, FoldedShape shape = {}, Deform deform = Deform::None,
            Index deformer = sNoIndex, Index material = sNoIndex);

        /// Poses one skinned mesh: its bone rows, and the box the pose reaches — the whole of what
        /// the host says about a body per frame, because its vertices are computed on the device
        /// from the bind pose. `bones.size()` must be the rig's `mBoneCount`. The mesh joins
        /// `getDeformed` for the frame unless nothing moved: rows equal to the ones already held
        /// write nothing, so an actor standing still costs no dispatch and no refit. Compared rather
        /// than trusted, because the walk poses every rig it meets.
        void poseRig(Index mesh, std::span<const Shaders::GpuBone> bones, const osg::BoundingBoxf& bounds);

        /// The same for a morphed mesh: one weight per target of its morph, the base's included and
        /// ignored, which is how `SceneUtil::MorphGeometry` numbers them.
        void poseMorph(Index mesh, std::span<const float> weights, const osg::BoundingBoxf& bounds);

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
        /// instead of rebuilding the list it was in.
        Index addInstance(const MeshInstance& instance);

        /// Appends one particle system's live sprites, and the emitter that names them. The sphere
        /// is derived here rather than passed in, so the rejection test a ray makes and the sprites
        /// it would then walk cannot disagree about where they are. Nothing is added for an emitter
        /// with no live particles.
        /// @param width how wide the quads are against their own axis, per unit of
        ///        `Sprite::mRadius`, or nought for sprites that face the eye. Every sprite carries
        ///        an axis where this is set and none where it is not — `SpriteEmitter::mWidth`.
        /// @param lighting the bake of `texture`'s alpha, or `sNoIndex`. `SpriteEmitter::mLighting`.
        void addEmitter(std::span<const Sprite> sprites, Index texture, bool additive, float width = 0.0f,
            Index lighting = sNoIndex);

        /// Drops every mesh and material the caller did not name — the only way a scene loses
        /// geometry, and nothing is renumbered by it: every bottom-level acceleration structure is
        /// named by a mesh index, and compacting is what made a cell boundary cost a full rebuild.
        /// Textures are not swept here: a material freed gives back what it named on its way out,
        /// as `setMaterial` and `TextureTable::drop` do, and its layer and mask runs go with it.
        /// Placements do not go: a slot is a name, and what it names has stopped moving.
        ///
        /// @param meshes every mesh to keep, each once, in any order.
        /// @param materials the same for materials.
        /// @return whether anything was freed. False is the ordinary frame, and it costs two
        ///         comparisons: a scene that lost nothing has as many survivors as it had entries.
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

        /// What a backend compares against to know whether the geometry or the textures it built
        /// from are still the ones the scene holds.
        std::uint64_t getStructureRevision() const { return mMeshes.getRevision() + mTextures.getRevision(); }

        /// The pose a deforming mesh was last given, and the weights a morphed one carries. Both
        /// cross two tables: the mesh row says where its run sits, and the deformers hold it.
        std::span<const Shaders::GpuBone> getMeshBones(Index mesh) const;
        std::span<const float> getMeshWeights(Index mesh) const;

        /// Every placement's own box, in the world.
        osg::BoundingBoxf getBounds() const;

        /// The same, clipped to `region` and with the water left out.
        ///
        /// **The sea is one sheet a hundred and fifty cells across**, so a caller asking how far the
        /// ground reaches would clear any threshold at every coastline.
        osg::BoundingBoxf getContentBoundsWithin(const osg::BoundingBoxf& region) const;

        /// Sorts the lights so that a frame's own order is a fact about the world. `SceneUploader`
        /// makes it at the one point every path passes, because a walk may run twice.
        void orderLights();

        /// Forgets what arrived and what was freed, which a hand-over does once it has read both.
        void clearArrivals();

    private:
        template <class Visit>
        void forEachPlacement(Visit&& visit) const;

        /// **The two borrowed tables first, because a member is constructed in declaration order.**
        /// `MeshTable` takes a `DeformerTable&` and `MaterialTable` a `TextureTable&`, so either
        /// moved below its borrower would bind a reference to storage no constructor had reached.
        TextureTable mTextures;
        DeformerTable mDeformers;

        /// Every mesh, and the shared buffers its triangles live in.
        MeshTable mMeshes{ mDeformers };

        /// The materials, their terrain layers and the weights those place.
        MaterialTable mMaterials{ mTextures };

        /// Where everything stands and which rows a backend has to write again.
        PlacementTable mPlacements;

        std::vector<Light> mLights;
        std::vector<Sprite> mSprites;
        std::vector<SpriteEmitter> mEmitters;
    };
}
