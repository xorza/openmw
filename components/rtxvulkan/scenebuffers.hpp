#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec2f>
#include <osg/Vec3f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/lightgrid.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/shaders/scene.h>

#include "blockedbuffer.hpp"
#include "buffer.hpp"
#include "frameslots.hpp"
#include "placing.hpp"
#include "slottable.hpp"
#include "spritebin.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;
    class SceneDesc;
    class SpriteBinPass;
    class SpriteShadePass;

    /// The tables a shader reads at a hit: the attributes, and the mesh, instance and material
    /// tables to find them through. Position fetch covers a normal; nothing covers a texture
    /// coordinate.
    class SceneBuffers
    {
    public:
        /// @param slots how many frames may be tracing this scene at once.
        SceneBuffers(const Device& device, Batch& batch, const SceneDesc& scene,
            std::span<const InstanceRecord> records, std::uint32_t slots);

        /// Takes in the attributes of the meshes the scene says arrived, and the layer and mask
        /// runs that arrived with them. The blocks are appended to rather than replaced, and a
        /// departure needs nothing here. Through `batch`, with frames in flight: a run an arrival
        /// was given may be one a material that went held until the last sweep, and the frame
        /// that shaded its hits can still be reading it. Ends in the barrier whatever reads them
        /// needs.
        void extend(Batch& batch, const SceneDesc& scene);

        /// Rewrites what a moving world changes — where things are and what is lit — leaving what
        /// it is made of alone: rebuilding all of it is tens of milliseconds on a nine-by-nine
        /// region. Those tables live in memory the host writes straight into, so this is a `memcpy`
        /// and not a staging buffer, a submit and a wait. Into `slot`'s copy of every table a frame
        /// writes, whose fence the caller waited; `SlotTable` and `SlotBlocks` keep each copy's
        /// account of what it is owed. `scene` must be the one the constructor was given, and
        /// `records` the rows the acceleration structure was placed with, so the motion transform a
        /// shader reads and the one an instance was placed with come out of the same arithmetic.
        ///
        /// @param changed the slots `updateInstanceRecords` wrote, which is the one list the rows
        ///        are driven by.
        void place(const SceneDesc& scene, std::span<const InstanceRecord> records, std::span<const Index> changed,
            const Placing& placing);

        /// Waits until nothing on the queue reads `slot`'s copy of any table `place` writes, ahead
        /// of the placement that writes it. Each copy carries its own stamp, so this waits for the
        /// submit that last read the copy whatever carried it — a frame's trace or a picture's.
        void finishReads(FrameSlot slot) const;

        /// The sprites `slot`'s copy holds and the emitters that placed them, for the bin a trace
        /// records against them (`SpriteBin::record`).
        SpriteSource describeSprites(FrameSlot slot) const;

        /// Where the lamps were binned, for the frame's block the pass writes: its geometry rides
        /// there, beside the sea's, and only the lists it made are tables.
        const LightGrid& getLightGrid() const { return mLightGrid; }

        /// The normals, for the pass that writes a deforming mesh's pose into a slot's copy of them.
        /// Their account is not what drives that pass — the positions' is, and one dispatch writes
        /// both — so nothing here is owed by a pose.
        SlotBlocks& getNormals() { return mNormalTable; }

        /// The tangents, which the same dispatch poses beside the normals.
        SlotBlocks& getTangents() { return mTangentTable; }

        /// Where every table this owns is, for the frame's block: those of `GpuTables` that are
        /// the scene's, with `slot`'s copy wherever a table has one per frame in flight. Addresses
        /// and never handles, because a shader constructs a reference from the block and reads; for
        /// the vertex attributes a table of addresses, one per block (`BlockedBuffer`). The
        /// blue-noise tile, the index blocks and the sprite bin's two are not the scene's and write
        /// their own.
        void describeTables(FrameSlot slot, Shaders::GpuTables& into) const;

        VkDeviceSize getBytes() const;

    private:
        /// What a frame writes whole, once per frame in flight. What is written by the row keeps
        /// its own account: `mInstanceTable`, `mMaterialTable`, `mNormalTable` and `mTangentTable`.
        struct Tables
        {
            Buffer mLights;
            Buffer mLightList;

            /// The sprites as the scene placed them, unshaded: what a trace's `SpriteBin` copies
            /// and shades for its own sun. Never read by a shader directly.
            Buffer mSprites;
            Buffer mEmitters;

            /// Where each medium and additive instance can be met, `PlacementTable::describePresences`,
            /// which the bin puts into the screen's tiles beside the sprites.
            Buffer mPresences;

            /// How many of each the copy holds, for the bin that copies them.
            std::uint32_t mSpriteCount = 0;
            std::uint32_t mEmitterCount = 0;
            std::uint32_t mPresenceCount = 0;

            /// What one copy of them occupies. Beside the declarations, because a table added above
            /// and forgotten here is a figure that quietly stops accounting for it, which two of
            /// them were.
            VkDeviceSize getBytes() const;
        };

        /// Reserves room for the scene's attributes, copies in the runs `meshes` names — into every
        /// copy of the normals — and rewrites the per-mesh row table. Per mesh and not per scene,
        /// because that is what an arrival is. Nothing is ordered here.
        void writeMeshes(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes);

        /// Stages the layer and mask runs that arrived — or a table whole where it had to be made
        /// again to hold them. Nothing is ordered here.
        void writeMaterialRuns(Batch& batch, const SceneDesc& scene);

        /// Writes the material rows `slot`'s copy owes.
        void shade(const SceneDesc& scene, FrameSlot slot);

        const Device& mDevice;

        // What the scene is made of, written on arrival and read by every frame: one copy, because
        // an arrival writes it on the queue, behind every frame in flight. The colours too, where
        // the normals are one per frame in flight: a skin recomputes a body's normals and never
        // repaints it.
        BlockedBuffer mTexCoords{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec2f) };

        /// The second set, in runs of their own: `Rtx::MeshRange::mSecondTexCoords`.
        BlockedBuffer mSecondTexCoords{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec2f) };
        BlockedBuffer mColours{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };

        /// A material's layers and the weights a layer places, one copy each for the same reason.
        /// Plain buffers grown by doubling, because a shader reaches a run by its offset from one
        /// address; the masks are megabytes, which is what a copy per frame in flight cost.
        Buffer mLayers;
        Buffer mMasks;

        /// One row a mesh slot, so a hit can turn its slot into offsets into the tables above.
        /// Rewritten whole whenever a mesh arrives or leaves, which is a few kilobytes.
        Buffer mMeshes;

        // Host-visible and rewritten from `place`, not uploaded once.
        PerSlot<Tables> mTables;

        std::vector<Shaders::GpuMesh> mMeshScratch;
        std::vector<Shaders::GpuPresence> mPresenceScratch;

        /// What the material table's runs stood at when they were last staged, which `shade` checks
        /// against: a run that arrived without an `extend` to stage it would be shaded stale for its
        /// life, and only a terrain chunk makes one — with its mesh, which is what brings the
        /// `extend`.
        std::uint64_t mStagedRuns = 0;

        /// What a hit turns its slot into: the mesh, the material, the opacity and the motion. Its
        /// own table rather than a field of `Tables`, because the copies and what each of them
        /// still owes are one thing and belong to one object (`SlotTable`).
        SlotTable<Shaders::GpuInstance> mInstanceTable;

        /// Every material the scene holds, and one row past them for the sentinel a placement with
        /// no material of its own wears.
        SlotTable<Shaders::GpuMaterial> mMaterialTable;

        /// Blocked like the geometry they belong to, so a scene that grows keeps the blocks it
        /// already has and adds one. One copy per frame in flight because a skinned body's normals
        /// are recomputed every frame — by `SkinPass`, into the copy the frame traces; the rest of a
        /// cell's are written once into every copy.
        SlotBlocks mNormalTable{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };

        /// The tangents, `Rtx::packTangent`'s words, blocked and copied as the normals are and for
        /// the same reason: a skinned body's are posed with its normals.
        SlotBlocks mTangentTable{ Shaders::VERTEX_BLOCK, sizeof(std::uint32_t) };

        /// Kept because the pass writes its geometry into the frame's block, which no table carries.
        LightGrid mLightGrid;
    };
}
