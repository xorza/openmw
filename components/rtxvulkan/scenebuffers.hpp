#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/lightgrid.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/spritetiles.hpp>
#include <components/rtx/wavespectrum.hpp>

#include "blockedbuffer.hpp"
#include "buffer.hpp"
#include "hostbuffer.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class SceneDesc;

    /// The tables a shader reads at a hit: what the triangle was, and how it is shaded.
    ///
    /// Positions and indices are already on the GPU for the acceleration structure to be built from,
    /// but a hit needs the *attributes* — and the mesh, instance and material tables to find them
    /// through. Position fetch covered a normal; nothing covers a texture coordinate.
    class SceneBuffers
    {
    public:
        /// @param indices the buffer `SceneAcceleration` already built from, borrowed rather than
        ///        uploaded again. It must outlive this.
        /// @param sea what the water is doing, which belongs to no cell: one table for the whole
        ///        world, animated by the time in the frame's constants rather than rebuilt. A state
        ///        with no height in it is a flat sea, which is what a test asserting an exact
        ///        transmittance needs.
        SceneBuffers(const Device& device, Batch& batch, const SceneDesc& scene,
            std::span<const InstanceRecord> records, const SeaState& sea = SeaState{});

        /// Takes in the attributes of the meshes the scene says arrived.
        ///
        /// The blocks are appended to rather than replaced, so nothing already written moves and
        /// nothing built from it has to be built again. A departure needs nothing here: a mesh slot
        /// with no geometry in it is never read.
        void extend(const SceneDesc& scene);

        /// Rewrites what a moving world changes, leaving what it is made of alone.
        ///
        /// **The split is the whole point of this class having two entry points.** Rebuilding all of
        /// it per frame was the largest single cost in the renderer — measured at twenty to
        /// twenty-seven milliseconds on a nine-by-nine region — and almost none of it had changed:
        /// the texture coordinates, the mesh table, the materials and the masks are what the scene is
        /// made of and only `setScene` can alter them.
        ///
        /// What does change is where things are, what is lit, and the vertices of anything skinned.
        /// Those live in memory the host writes straight into, so this is a `memcpy` and not a
        /// staging buffer, a copy command, a submit and a wait on the queue.
        ///
        /// `scene` must be the one the constructor was given. `records` are the rows the
        /// acceleration structure was placed with, handed in rather than made again: the motion
        /// transform a shader reads and the one an instance was placed with have to come out of the
        /// same arithmetic, and two places computing an inverse is two places to get it wrong — as
        /// well as thousands of inversions a frame done twice for one answer.
        void place(const SceneDesc& scene, std::span<const InstanceRecord> records, const SeaState& sea);

        SceneBuffers(const SceneBuffers&) = delete;
        SceneBuffers& operator=(const SceneBuffers&) = delete;

        /// Where each blocked table's blocks are, as a shader reads them.
        ///
        /// **Tables of addresses and not the data.** The vertex attributes are lists of blocks, so
        /// what a shader binds is where the blocks are; it resolves a global id to one of them
        /// itself. See `BlockedBuffer`.
        VkBuffer getNormalBlocks() const { return mNormals.getTable(); }
        VkBuffer getTexCoordBlocks() const { return mTexCoords.getTable(); }
        VkBuffer getMeshes() const { return mMeshes.getHandle(); }
        VkBuffer getInstances() const { return mInstances.getHandle(); }
        VkBuffer getMaterials() const { return mMaterials.getHandle(); }
        VkBuffer getLayers() const { return mLayers.getHandle(); }
        VkBuffer getMasks() const { return mMasks.getHandle(); }
        VkBuffer getLights() const { return mLights.getHandle(); }
        VkBuffer getLightOffsets() const { return mLightOffsets.getHandle(); }
        VkBuffer getLightIndices() const { return mLightIndices.getHandle(); }
        VkBuffer getSprites() const { return mSprites.getHandle(); }
        VkBuffer getEmitters() const { return mEmitters.getHandle(); }

        /// Bins this scene's sprites into the screen tiles of the camera about to trace them.
        ///
        /// **From the frame and not from the placement**, because the binning is in screen space and
        /// the camera does not exist until the frame does. `place` wrote the sprites; this reads the
        /// copy of them kept beside the buffer.
        void binSprites(const osg::Vec3f& origin, const Shaders::Camera& camera);

        VkBuffer getSpriteTileOffsets() const { return mSpriteTileOffsets.getHandle(); }
        VkBuffer getSpriteTileIndices() const { return mSpriteTileIndices.getHandle(); }

        /// Where the lamps were binned, for the constants the pass pushes.
        const LightGrid& getLightGrid() const { return mLightGrid; }

        /// The grid's geometry, as the shader reads it.
        VkBuffer getGrid() const { return mGrid.getHandle(); }
        VkBuffer getWaves() const { return mWaves.getHandle(); }

        VkDeviceSize getBytes() const;

    private:
        /// Grows one of this object's tables, which all take the same device and the same usage.
        ///
        /// A thin name over `growTo` — the rule that a table is never nothing lives there, and this
        /// only saves every call site from repeating what is the same for all of them.
        void reserve(HostBuffer& held, VkDeviceSize bytes);

        /// Reserves room for the scene's attributes, copies in the runs `meshes` names, and rewrites
        /// the per-mesh row table.
        ///
        /// **Per mesh and not per scene**, because that is what an arrival is: the blocks already
        /// hold everything else, and rewriting them would be rewriting what nothing changed.
        void writeMeshes(const SceneDesc& scene, std::span<const Index> meshes);

        /// Rewrites the shading tables if the scene says they have changed, and nothing otherwise.
        void shade(const SceneDesc& scene);

        const Device* mDevice = nullptr;

        // What the scene is made of. Written once, through a staging copy, because nothing rewrites
        // them and device-only memory is the faster place for the device to read.
        BlockedBuffer mTexCoords{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec2f) };

        // **Host-visible and rewritten from `place`, not uploaded once.** Anything that animates a
        // state set gives the mirror a new material every frame — OpenMW's water cycles thirty-two
        // of them — and a table that could only be filled at construction made that a reason to
        // rebuild the whole scene. A few kilobytes written when the scene says they changed is what
        // it is actually worth.
        HostBuffer mMaterials;
        HostBuffer mLayers;
        HostBuffer mMasks;

        /// The shading revision these three were last written at, so a frame that changed nothing
        /// writes nothing.
        std::uint64_t mShaded = 0;

        std::vector<Shaders::GpuMesh> mMeshScratch;
        std::vector<Shaders::GpuMaterial> mMaterialScratch;
        std::vector<Shaders::GpuLayer> mLayerScratch;

        // What a moving world changes. Written straight into video memory every frame.
        //
        // The normals are here for the sake of a few dozen of them: a skinned body's are recomputed
        // per frame and the rest of a cell's never change, so the buffer is filled once and then
        // written a mesh at a time.
        //
        // **Blocked like the geometry they belong to**, so a scene that grows keeps the blocks it
        // already has and adds one.
        BlockedBuffer mNormals{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };

        /// One row a mesh slot, so a hit can turn its slot into offsets into the tables above.
        /// Rewritten whole whenever a mesh arrives or leaves, which is a few kilobytes.
        HostBuffer mMeshes;

        HostBuffer mInstances;
        HostBuffer mLights;
        HostBuffer mLightOffsets;
        HostBuffer mGrid;
        HostBuffer mLightIndices;
        HostBuffer mWaves;
        HostBuffer mSprites;
        HostBuffer mEmitters;
        HostBuffer mSpriteTileOffsets;
        HostBuffer mSpriteTileIndices;

        // Refilled per placement rather than reallocated: a scene is tens of thousands of instances
        // and this is the frame path.
        std::vector<Shaders::GpuInstance> mInstanceScratch;
        std::vector<Shaders::GpuLight> mLightScratch;

        SpriteTiles mSpriteTiles;

        std::vector<Shaders::GpuSprite> mSpriteScratch;
        std::vector<Shaders::GpuEmitter> mEmitterScratch;

        /// Kept because the pass needs its geometry, which no buffer carries.
        LightGrid mLightGrid;
    };
}
