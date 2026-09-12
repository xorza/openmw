#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec2f>
#include <osg/Vec3f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/lightgrid.hpp>
#include <components/rtx/shaders/camera.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/spritelistsize.hpp>

#include "blockedbuffer.hpp"
#include "buffer.hpp"
#include "frameslots.hpp"
#include "placing.hpp"
#include "slottable.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;
    class Graveyard;
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
            std::span<const InstanceRecord> records, std::uint32_t slots, Graveyard& graveyard);

        /// Takes in the attributes of the meshes the scene says arrived. The blocks are appended to
        /// rather than replaced, and a departure needs nothing here. With nothing in flight, which
        /// the caller guarantees: an arrival writes every copy of the normals and the whole mesh
        /// table.
        void extend(Batch& batch, const SceneDesc& scene, Graveyard& graveyard);

        /// Rewrites what a moving world changes — where things are and what is lit — leaving what
        /// it is made of alone: rebuilding all of it is tens of milliseconds on a nine-by-nine
        /// region. Those tables live in memory the host writes straight into, so this is a `memcpy`
        /// and not a staging buffer, a submit and a wait. Into `slot`'s copy of every table a frame
        /// writes, which the caller has waited the fence of; `SlotTable` and `SlotBlocks` keep each
        /// copy's account of what it is owed.
        ///
        /// `scene` must be the one the constructor was given. `records` are the rows the
        /// acceleration structure was placed with, so the motion transform a shader reads and the
        /// one an instance was placed with come out of the same arithmetic.
        ///
        /// @param changed the slots `updateInstanceRecords` wrote, which is the one list the rows
        ///        are driven by.
        void place(const SceneDesc& scene, std::span<const InstanceRecord> records, std::span<const Index> changed,
            FrameSlot slot, Graveyard& graveyard);

        /// Shades this scene's sprites against the frame's sun, writes them, and records the bin
        /// of them into the screen tiles of the camera about to trace them. From the frame and not
        /// from the placement, because both the sun and the camera are the frame's. Recorded into
        /// `placing.mCommands` ahead of the trace that reads the tiles, and grows the list first
        /// from what this copy's last bin reported it needed, so nothing between this and the trace
        /// moves a table.
        void binSprites(const SpriteShadePass& shading, const SpriteBinPass& pass, const osg::Vec3f& origin,
            const Shaders::Camera& camera, const osg::Vec3f& toSun, const Placing& placing);

        /// Where the lamps were binned, for the frame's block the pass writes: its geometry rides
        /// there, beside the sea's, and only the lists it made are tables.
        const LightGrid& getLightGrid() const { return mLightGrid; }

        /// The normals, for the pass that writes a deforming mesh's pose into a slot's copy of them.
        /// Their account is not what drives that pass — the positions' is, and one dispatch writes
        /// both — so nothing here is owed by a pose.
        SlotBlocks& getNormals() { return mNormalTable; }

        /// Where every table this owns is, for the frame's block: the twelve of `GpuTables` that are
        /// the scene's, with `slot`'s copy wherever a table has one per frame in flight. Addresses
        /// and never handles, because a shader constructs a reference from the block and reads; for
        /// the vertex attributes a table of addresses, one per block (`BlockedBuffer`). The
        /// blue-noise tile and the index blocks are not the scene's and write their own.
        void describeTables(FrameSlot slot, Shaders::GpuTables& into) const;

        VkDeviceSize getBytes() const;

    private:
        /// What a frame writes whole, once per frame in flight. What is written by the row keeps
        /// its own account: `mInstanceTable`, `mMaterialTable` and `mNormalTable`.
        struct Tables
        {
            Buffer mLayers;
            Buffer mMasks;
            Buffer mLights;
            Buffer mLightList;
            Buffer mSprites;
            Buffer mEmitters;

            /// The sprite tiles' list, made on the device by `SpriteBinPass` and never written by
            /// the host: `tiles + 1` starts, then the runs, in `RunList`'s shape.
            Buffer mSpriteTileList;

            /// One rectangle of tiles per sprite, the bin's own scratch between its dispatches.
            Buffer mSpriteRects;

            /// One depth key per sprite per light, the shading's own scratch inside its dispatch.
            /// `Shaders::SpriteShadeConstants::mOrder` says how the two lights share it.
            Buffer mSpriteOrder;

            /// How many entries the last bin into this copy came to, written by the pass and read
            /// back here before the next bin. Staging, because it is the one table the host reads.
            Buffer mSpriteBinReport;

            /// How long `mSpriteTileList` is and how much of it a bin may fill. Grown from the
            /// report and never shrunk, so the list settles at its high-water mark like every
            /// other table. `SpriteListSize` says why the two numbers are one object.
            SpriteListSize mSpriteListSize;

            /// What one copy of them occupies.
            ///
            /// **Beside the declarations, because a table added above and forgotten here is a
            /// figure that quietly stops accounting for it.** Two of them already were.
            VkDeviceSize getBytes() const;
        };

        /// Grows one of this object's tables to exactly `bytes`, burying what that displaced.
        ///
        /// A thin name over `growTo` — the rule that a table is never nothing lives there, and this
        /// only saves every call site from repeating what is the same for all of them.
        void reserve(Buffer& held, VkDeviceSize bytes, Graveyard& graveyard);

        /// Reserves room for the scene's attributes, copies in the runs `meshes` names — into every
        /// copy of the normals — and rewrites the per-mesh row table. Per mesh and not per scene,
        /// because that is what an arrival is.
        void writeMeshes(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes, Graveyard& graveyard);

        /// Writes the material rows `slot`'s copy owes, and the layer and mask runs that arrived into
        /// every copy — or a table whole where it had to be made again to hold them.
        void shade(const SceneDesc& scene, FrameSlot slot, Graveyard& graveyard);

        const Device* mDevice = nullptr;
        std::uint32_t mSlots = 1;

        // What the scene is made of, written on arrival and read by every frame: one copy, because
        // an arrival waits for the frames in flight before it writes. The colours too, where the
        // normals are one per frame in flight: a skin recomputes a body's normals and never
        // repaints it.
        BlockedBuffer mTexCoords{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec2f) };
        BlockedBuffer mColours{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };

        /// One row a mesh slot, so a hit can turn its slot into offsets into the tables above.
        /// Rewritten whole whenever a mesh arrives or leaves, which is a few kilobytes.
        Buffer mMeshes;

        // Host-visible and rewritten from `place`, not uploaded once: anything that animates a
        // state set gives the mirror a new material every frame, and only the rows the scene says
        // it wrote go over — the masks are megabytes and a flipbook turning changes none of them.
        std::array<Tables, sFrameSlots> mTables;

        std::vector<Shaders::GpuMesh> mMeshScratch;
        std::vector<Shaders::GpuLayer> mLayerScratch;

        /// What a hit turns its slot into: the mesh, the material, the opacity and the motion.
        ///
        /// **Its own table rather than a field of `Tables`**, because the copies and what each of
        /// them still owes are one thing and belong to one object. `SlotTable` says why.
        SlotTable<Shaders::GpuInstance> mInstanceTable;

        /// Every material the scene holds, and one row past them for the sentinel a placement with
        /// no material of its own wears.
        SlotTable<Shaders::GpuMaterial> mMaterialTable;

        /// **Blocked like the geometry they belong to**, so a scene that grows keeps the blocks it
        /// already has and adds one. One copy per frame in flight because a skinned body's normals
        /// are recomputed every frame — by `SkinPass`, into the copy the frame traces; the rest of a
        /// cell's are written once into every copy.
        SlotBlocks mNormalTable{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };

        // Refilled per placement rather than reallocated: a scene is thousands of these and this is
        // the frame path.
        std::vector<Shaders::GpuLight> mLightScratch;

        std::vector<Shaders::GpuSprite> mSpriteScratch;
        std::vector<Shaders::GpuEmitter> mEmitterScratch;

        /// Kept because the pass writes its geometry into the frame's block, which no table carries.
        LightGrid mLightGrid;
    };
}
