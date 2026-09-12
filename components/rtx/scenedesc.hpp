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
#include "index.hpp"
#include "light.hpp"
#include "material.hpp"
#include "materialtable.hpp"
#include "mesharrays.hpp"
#include "meshinstance.hpp"
#include "meshrange.hpp"
#include "meshtable.hpp"
#include "placementtable.hpp"
#include "runallocator.hpp"
#include "scenetables.hpp"
#include "shaders/skinning.h"
#include "shapefold.hpp"
#include "sprite.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    /// The two tables that the mesh table and the material table borrow.
    ///
    /// **A base and not two more members, because a base is constructed before every member of the
    /// class that carries it, whatever order those members are declared in.** `MeshTable` takes a
    /// `DeformerTable&` and `MaterialTable` a `TextureTable&`, so as plain members these two would
    /// have to be declared before their borrowers with nothing but a comment saying so — and a
    /// member moved would bind a reference to storage no constructor had reached.
    struct LentTables
    {
        /// What poses the deforming meshes, and the poses themselves. Its own type, for the reason
        /// the tables that borrow it are: a rig, the meshes counted on it and the runs behind both
        /// are one invariant.
        ///
        /// A mesh stands its deformer as it arrives and releases one as it goes, and the count is
        /// this table's.
        DeformerTable mDeformers;

        /// Every texture the scene names, and what still names each. Its own type, because a
        /// reference-counted table with a free list and two lookups into it is a thing with an
        /// invariant rather than a set of parallel vectors.
        ///
        /// A material names texture slots and gives them back as it is rewritten and as it is
        /// swept, and the count is this table's.
        TextureTable mTextures;
    };

    class SceneDesc : private LentTables
    {
    public:
        /// What a mesh's geometry may not straddle — `MeshTable::sVertexBlock` says why.
        static constexpr Index sVertexBlock = MeshTable::sVertexBlock;
        static constexpr Index sIndexBlock = MeshTable::sIndexBlock;

        /// Copies the vertex data into the shared buffers and returns the new mesh's index.
        ///
        /// Every attribute but `MeshArrays::mPositions` may be empty; when one is not it must match
        /// the positions in length, and `MeshArrays::mIndices` must be a whole number of triangles
        /// addressing only those vertices. All of that is a contract on the caller, so it is
        /// asserted rather than reported.
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
        Index addMesh(const MeshArrays& arrays, FoldedShape shape = {}, Deform deform = Deform::None,
            Index deformer = sNoIndex, Index material = sNoIndex);

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

        void addLight(const Light& light);

        /// Whether a hold on a mesh or a material went to nought since the last `release`.
        bool hasDroppedHolds() const;

        /// Places `instance` in a slot and returns it.
        ///
        /// **The slot is the placement's name for as long as it stands.** It is the custom index a
        /// hit reads back, the row a shader looks its material up in, and — because it outlives the
        /// walk that made it — what lets a mirror move a placement instead of rebuilding the list
        /// it was in. A slot freed by `placements().drop` is handed out again; one that is still
        /// standing never is.
        Index addInstance(const MeshInstance& instance);

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
        /// animation stops naming an image and the same thing `TextureTable::drop` does for an emitter's
        /// sprite. Sweeping them instead meant asking on the frames a mesh or a material happened to
        /// die as well, and a texture that stopped being named on any other frame was never noticed.
        ///
        /// Layers and masks have no keep set either: they belong to the material that owns them, so
        /// a freed material hands both runs back to their allocators on its way out. A terrain
        /// chunk's masks are tens of kilobytes and a player can cross the whole continent through
        /// one `SceneDesc`, so leaving them to the sweep that eventually drops the material is a
        /// session-long growth.
        ///
        /// **Placements do not go**, and nothing carries them anywhere either: a slot is a name, and
        /// what it names has stopped moving.
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
        /// `addInstance` for one that has appeared, `placements().move` for one that has shifted,
        /// `placements().drop` for one that has gone — because a slot index is what a hit reads back and
        /// because a world of fifty thousand placements of which three hundred move should cost
        /// three hundred. Everything above is small enough per frame that rebuilding it is cheaper
        /// than reconciling it.
        void clearPlacement();

        /// The tables, for whoever builds the scene to write straight into.
        ///
        /// **Only what crosses tables stays on this class** — `addMesh` and `addInstance` for the
        /// asserts they make between two tables, the poses and `setMaterial` for what they hand
        /// on to the placements, the sweep, the lights and the emitters. Everything that reads or
        /// writes one table is asked of that table: a forwarder is a second place a reader has to
        /// look for the same answer.
        MeshTable& meshes() { return mMeshTable; }
        MaterialTable& materials() { return mMaterialTable; }
        TextureTable& textures() { return mTextures; }
        PlacementTable& placements() { return mPlacements; }
        DeformerTable& deformers() { return mDeformers; }

        /// The read side, for whoever is handed the scene rather than building it. `SceneTables`
        /// says what a reader may do with it.
        ///
        /// **Every span a table hands out is into storage that grows, and lives until it does.** A
        /// span is valid until the next `add` into its table — `addMesh` grows the geometry and the
        /// mesh table, `addEmitter` the sprites and the emitters — and until `clearPlacement`, which
        /// empties the per-frame ones. Take it after the add and never in the same expression as
        /// one: a span read beside an add is sequenced before it, and indexes a table that has
        /// moved.
        SceneTables getTables() const
        {
            return SceneTables{
                .mMeshes = mMeshTable,
                .mDeformers = mDeformers,
                .mPlacements = mPlacements,
                .mMaterials = mMaterialTable,
                .mTextures = mTextures,
                .mLights = mLights,
                .mSprites = mSprites,
                .mEmitters = mEmitters,
            };
        }

        /// Sorts the lights so that a frame's own order is a fact about the world.
        ///
        /// **The one call a reader needs that is not a table.** `SceneUploader` makes it at the one
        /// point every path passes, because a walk may run twice and a light met by the second
        /// would otherwise stand outside an order the first had settled.
        void orderLights();

        /// Forgets what arrived and what was freed, which a hand-over does once it has read both.
        void clearArrivals();

    private:
        /// Every mesh, and the shared buffers its triangles live in.
        MeshTable mMeshTable{ mDeformers };

        /// Where everything stands and which rows a backend has to write again. Its own type,
        /// because slots that are never moved, a free list and two change lists are one invariant.
        PlacementTable mPlacements;

        std::vector<Light> mLights;
        std::vector<Sprite> mSprites;
        std::vector<SpriteEmitter> mEmitters;

        /// The materials, their terrain layers and the weights those place. Its own type, for the
        /// reason the tables above are: a row, the runs it names and what those name are one
        /// invariant rather than ten members held in step by hand.
        MaterialTable mMaterialTable{ mTextures };
    };
}
