#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/lightgrid.hpp>
#include <components/rtx/shaders/camera.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/spriteshade.hpp>

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

    /// The tables a shader reads at a hit: what the triangle was, and how it is shaded.
    ///
    /// Positions and indices are already on the GPU for the acceleration structure to be built from,
    /// but a hit needs the *attributes* — and the mesh, instance and material tables to find them
    /// through. Position fetch covered a normal; nothing covers a texture coordinate.
    class SceneBuffers
    {
    public:
        /// @param slots how many frames may be tracing this scene at once: `sFrameSlots` for the
        ///        world, and one for a picture inside the interface, which is traced and waited for
        ///        before anything else touches it.
        SceneBuffers(const Device& device, const SceneDesc& scene, std::span<const InstanceRecord> records,
            std::uint32_t slots, Graveyard& graveyard);

        /// Takes in the attributes of the meshes the scene says arrived.
        ///
        /// The blocks are appended to rather than replaced, so nothing already written moves and
        /// nothing built from it has to be built again. A departure needs nothing here: a mesh slot
        /// with no geometry in it is never read.
        ///
        /// **With nothing in flight**, which the caller guarantees: an arrival writes every copy of
        /// the normals and the whole mesh table, and a frame still reading either would see it torn.
        void extend(const SceneDesc& scene, Graveyard& graveyard);

        /// Rewrites what a moving world changes, leaving what it is made of alone.
        ///
        /// **The split is the whole point of this class having two entry points.** Rebuilding all of
        /// it per frame was the largest single cost in the renderer — measured at twenty to
        /// twenty-seven milliseconds on a nine-by-nine region — and almost none of it had changed:
        /// the texture coordinates and the mesh table are what the scene is made of and only an
        /// arrival can alter them, and the materials, the layers and the masks change by the row
        /// and the run, which is what the scene reports and what `shade` writes.
        ///
        /// What does change is where things are and what is lit. Those live in memory the host
        /// writes straight into, so this is a `memcpy` and not a staging buffer, a copy command, a
        /// submit and a wait on the queue. The vertices of anything skinned change too, and those
        /// `SkinPass` writes on the device.
        ///
        /// **Into `slot`'s copy of every table a frame writes**, which the frame after next reads
        /// again and no frame in between: the caller has waited that frame's fence. Every table
        /// keeps its own copies and its own account of what each of them is owed, which is what
        /// `SlotTable` and `SlotBlocks` are for.
        ///
        /// `scene` must be the one the constructor was given. `records` are the rows the
        /// acceleration structure was placed with, handed in rather than made again: the motion
        /// transform a shader reads and the one an instance was placed with have to come out of the
        /// same arithmetic, and two places computing an inverse is two places to get it wrong — as
        /// well as thousands of inversions a frame done twice for one answer.
        ///
        /// @param changed the slots `updateInstanceRecords` wrote, which is the one list the rows
        ///        are driven by. Whether a copy is then behind is `mInstanceTable`'s to know.
        void place(const SceneDesc& scene, std::span<const InstanceRecord> records, std::span<const Index> changed,
            std::uint32_t slot, Graveyard& graveyard);

        SceneBuffers(const SceneBuffers&) = delete;
        SceneBuffers& operator=(const SceneBuffers&) = delete;

        /// Shades this scene's sprites against the frame's sun, writes them, and records the bin
        /// of them into the screen tiles of the camera about to trace them.
        ///
        /// **From the frame and not from the placement**, because the binning is in screen space and
        /// the camera does not exist until the frame does. `place` converted the sprites; this
        /// writes the copy of them kept beside the buffer and hands `pass` the tables to bin them
        /// into.
        ///
        /// **Recorded into `placing.mCommands` ahead of the trace that reads the tiles**, and after
        /// that placement's copy of them is nothing's to read — which for a frame is the fence the
        /// frame before last signalled, and for a picture inside the interface every frame's. Grows
        /// the list first, from what this copy's last bin reported it needed, so nothing between
        /// this and the trace moves a table.
        void binSprites(const SpriteBinPass& pass, const osg::Vec3f& origin, const Shaders::Camera& camera,
            const osg::Vec3f& toSun, const Placing& placing);

        /// Where the lamps were binned, for the frame's block the pass writes: its geometry rides
        /// there, beside the sea's, and only the lists it made are tables.
        const LightGrid& getLightGrid() const { return mLightGrid; }

        /// The normals, for the pass that writes a deforming mesh's pose into a slot's copy of them.
        /// Their account is not what drives that pass — the positions' is, and one dispatch writes
        /// both — so nothing here is owed by a pose.
        SlotBlocks& getNormals() { return mNormalTable; }

        /// Where `mesh`'s texture coordinates start, for the bake that reads a cutout's mask
        /// through them. One address covers the run: a mesh never straddles a block.
        VkDeviceAddress getTexCoords(const MeshRange& mesh) const { return mTexCoords.addressOf(mesh.mVertexOffset); }

        /// Where every table this owns is, for the frame's block: the twelve of `GpuTables` that are
        /// the scene's, with `slot`'s copy wherever a table has one per frame in flight.
        ///
        /// **Addresses and never handles**, because nothing binds a table: a shader constructs a
        /// reference from the block and reads. For the vertex attributes the address is a table of
        /// addresses, one per block, and the shader resolves a global id to one of them itself. See
        /// `BlockedBuffer`.
        ///
        /// **The two it leaves alone are not the scene's.** The blue-noise tile is the pass's and
        /// the index blocks are the acceleration structure's, and each of those writes its own.
        void describeTables(std::uint32_t slot, Shaders::GpuTables& into) const;

        VkDeviceSize getBytes() const;

    private:
        /// What a frame writes whole, once per frame in flight.
        ///
        /// **What is written by the row has left this**, because a table and the account of what
        /// each copy of it still owes are one thing: `mInstanceTable`, `mMaterialTable` and
        /// `mNormalTable` keep their own. These are the ones a placement fills from end to end.
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

            /// One rectangle of tiles per sprite, the pass's own scratch between its dispatches.
            Buffer mSpriteRects;

            /// How many entries the last bin into this copy came to, written by the pass and read
            /// back here before the next bin. Staging, because it is the one table the host reads.
            Buffer mSpriteBinReport;

            /// How many entries `mSpriteTileList` has room for after its starts. Grown from the
            /// report and never shrunk, so the list settles at its high-water mark like every
            /// other table.
            std::uint32_t mSpriteEntries = 0;

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
        /// copy of the normals — and rewrites the per-mesh row table.
        ///
        /// **Per mesh and not per scene**, because that is what an arrival is: the blocks already
        /// hold everything else, and rewriting them would be rewriting what nothing changed.
        void writeMeshes(const SceneDesc& scene, std::span<const Index> meshes, Graveyard& graveyard);

        /// Writes the material rows `slot`'s copy owes, and the layer and mask runs that arrived into
        /// every copy — or a table whole where it had to be made again to hold them.
        void shade(const SceneDesc& scene, std::uint32_t slot, Graveyard& graveyard);

        const Device* mDevice = nullptr;
        std::uint32_t mSlots = 1;

        // What the scene is made of, written on arrival and read by every frame: one copy, because
        // an arrival waits for the frames in flight before it writes.
        BlockedBuffer mTexCoords{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec2f) };

        /// One row a mesh slot, so a hit can turn its slot into offsets into the tables above.
        /// Rewritten whole whenever a mesh arrives or leaves, which is a few kilobytes.
        Buffer mMeshes;

        // **Host-visible and rewritten from `place`, not uploaded once.** Anything that animates a
        // state set gives the mirror a new material every frame — OpenMW's water cycles thirty-two
        // of them — and a table that could only be filled at construction made that a reason to
        // rebuild the whole scene. The rows the scene says it wrote are what go over, and only
        // those: the masks are megabytes and a flipbook turning changes none of them.
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

        SpriteShade mSpriteShade;

        std::vector<Shaders::GpuSprite> mSpriteScratch;
        std::vector<Shaders::GpuEmitter> mEmitterScratch;

        /// Kept because the pass writes its geometry into the frame's block, which no table carries.
        LightGrid mLightGrid;
    };
}
