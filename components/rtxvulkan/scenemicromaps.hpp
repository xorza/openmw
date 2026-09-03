#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <osg/Vec4f>

#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/micromap.h>

#include "structurestorage.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class GpuTimer;
    class Graveyard;
    class MicromapPass;
    class SceneAcceleration;
    class SceneBuffers;
    class TextureArray;

    /// One scene's opacity micromaps: one per cutout mesh, baked as the mesh arrives and attached
    /// to its structure for as long as the mesh stands.
    ///
    /// **What a micromap is for.** A third of the instances in a town are cutouts, and every ray
    /// that met one — the eye's, the bounce, every shadow ray, the fog volume's probes — stopped
    /// for a texture fetch to learn whether it had landed in a hole. The mask is resolved once
    /// instead, per microtriangle, when the mesh arrives; traversal then walks through the holes
    /// and commits the leaves without asking, and stops only where the bake could not decide.
    /// `micromap.h` says what a microtriangle promises and how the states are ordered;
    /// `MicromapPass` decides them; this is what chooses each triangle's level, builds the micromap
    /// the states become, and keeps it.
    ///
    /// **Which meshes get one.** A mesh whose material is a cutout, is not translucent, is not
    /// animated, and whose mask is a texture the array holds — `MeshRange::mMaterial` is the one
    /// material a static mesh is worn with, and `Material::mAnimated` is what refuses a mask that
    /// scrolls. A placement the game is fading keeps its mesh's micromap and reads its leaves
    /// through the any-hit from a row forced non-opaque, which `SceneAcceleration::placeRow` says
    /// why.
    ///
    /// **Storage like the structures'**: opaque driver objects at 256-byte offsets in blocks the
    /// scene owns, rooms given back through the graveyard, because a frame in flight may still be
    /// tracing a structure that references one.
    class SceneMicromaps
    {
    public:
        /// Throws where the device cannot cut a triangle as finely as `MICROMAP_LEVEL_MAX`, which
        /// is the one limit the bake reads and is refused rather than clamped to.
        explicit SceneMicromaps(const Device& device);
        ~SceneMicromaps();

        SceneMicromaps(const SceneMicromaps&) = delete;
        SceneMicromaps& operator=(const SceneMicromaps&) = delete;

        /// Bakes and builds a micromap for each of `meshes` that takes one, into `batch`: the
        /// levels and the usage counts on the host, the states in one dispatch a mesh, and every
        /// micromap built at once with one scratch — bracketed by a barrier from the kernel's writes
        /// to the build and one from the build to the structures built over it.
        ///
        /// **After the textures are written and before the structures are built**, in the same
        /// batch: the kernel samples the array the arrival just filled, and `describe` is what the
        /// structure's build chains. A slot handed out again arrives holding different geometry,
        /// so whatever it carried goes to `graveyard` first.
        ///
        /// @param timer the frame the arrival lands in, so the bake and the builds are one zone of
        ///        that frame's report — `SceneAcceleration::buildArrived` says why — or null for a
        ///        load and for a picture inside the interface, which are not timed.
        void bake(Batch& batch, const MicromapPass& pass, const SceneDesc& scene, const SceneBuffers& buffers,
            const SceneAcceleration& acceleration, const TextureArray& textures, std::span<const Index> meshes,
            GpuTimer* timer, Graveyard& graveyard);

        /// Whether `mesh` carries one.
        bool has(Index mesh) const { return mesh < mMeshes.size() && mMeshes[mesh].mHandle != VK_NULL_HANDLE; }

        /// What a triangle geometry chains through `pNext` to trace against `mesh`'s micromap:
        /// every triangle owns the entry at its own index, so there is no index buffer, and the
        /// usage counts are the ones the micromap was built with. `has(mesh)` must hold.
        ///
        /// **Points into this object, and stays right until the next `bake`**: the counts are
        /// kept per mesh in a table that only a bake grows, and a bake runs before any build that
        /// reads what it handed out. A refit describes the same micromap the build did, which an
        /// update requires — a rig's texture coordinates never move, so the micromap it was built
        /// with stays right through every pose.
        VkAccelerationStructureTrianglesOpacityMicromapEXT describe(Index mesh) const;

        /// Gives back the micromaps of `meshes`, through `graveyard`. **Idempotent**, for the reason
        /// `SceneAcceleration::release` is: both the frame that places and the one that appends run
        /// it, and a slot that carries nothing costs two comparisons.
        void release(std::span<const Index> meshes, Graveyard& graveyard);

        /// Throws where a material a micromap was baked against has been rewritten under it in a
        /// way that changes the bake — its diffuse, its cutoff, its transform, its translucency.
        ///
        /// **A hard failure and not a rebuild, because it cannot happen and must not pass.** Only
        /// an animated material is ever rewritten, and no animated material is ever baked; a
        /// rewrite that reached here would leave every placement of the mesh traced against a mask
        /// it no longer carries. Rebuilding the structure bare would also mean renumbering every
        /// row that places it, on the frame path, for a case the loader rules out — so the number
        /// that says the rule holds is a throw naming the material, once a frame and costing
        /// nothing on the frames a flipbook turns.
        void check(const SceneDesc& scene);

        /// What the micromaps hold, apart from the structures they are attached to.
        VkDeviceSize getBytes() const { return mStorage.getBytes(); }

        /// Cutout meshes built bare because the slot their mask lives in held no texture. See
        /// `SceneStats::mMicromapsUntextured`.
        std::uint32_t getUntexturedCount() const { return mUntextured; }

    private:
        /// What a mesh's bake read of its material, kept so a rewrite can be told from a no-op.
        struct Baked
        {
            Index mMaterial = sNoIndex;
            Index mDiffuse = sNoIndex;
            float mCutoff = 0.0f;
            osg::Vec4f mTransform;
            bool mTranslucent = false;

            bool operator==(const Baked& other) const = default;
        };

        static Baked bakedOf(const Material& material, Index index);

        /// How many levels a mesh may hold triangles at, and so how many usage counts one needs.
        static constexpr std::uint32_t sLevelCount = Shaders::MICROMAP_LEVEL_MAX - Shaders::MICROMAP_LEVEL_MIN + 1;

        /// One mesh slot: the micromap, its room, and what its build was told.
        struct MeshMicromap
        {
            VkMicromapEXT mHandle = VK_NULL_HANDLE;
            StructureRoom mRoom;

            /// One entry per level in use, which the build and every attachment must be given
            /// exactly. **A fixed array and not a run in a shared vector**, because `describe` hands
            /// out a pointer to it and a vector that grew would leave the pointer behind.
            std::array<VkMicromapUsageEXT, sLevelCount> mUsage{};
            std::uint32_t mUsageCount = 0;

            Baked mBaked;
        };

        /// One mesh of a bake in progress, from the plan to the record.
        struct Planned
        {
            Index mMesh = sNoIndex;

            /// Where its triangles start in `mTriangleScratch`.
            std::uint32_t mTriangleAt = 0;

            /// Its offsets in the batch's data, triangle-array and scratch buffers.
            VkDeviceSize mData = 0;
            VkDeviceSize mTriangles = 0;
            VkDeviceSize mScratch = 0;

            VkMicromapBuildSizesInfoEXT mSizes{};
        };

        /// Buries what `mesh` carries, if anything.
        void drop(Index mesh, Graveyard& graveyard);

        const Device& mDevice;

        StructureStorage mStorage{ VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            "micromaps" };

        /// Every mesh slot, grown to the scene's table by `bake` and never shrunk.
        std::vector<MeshMicromap> mMeshes;

        // Refilled per bake rather than reallocated: an arrival is tens of these.
        std::vector<Planned> mPlanned;
        std::vector<VkMicromapTriangleEXT> mTriangleScratch;
        std::vector<VkMicromapBuildInfoEXT> mBuildScratch;
        std::vector<std::uint32_t> mSlotScratch;

        /// One flag per material slot, for `check` to find the rewritten ones in one pass.
        std::vector<std::uint8_t> mWrittenScratch;

        std::uint32_t mUntextured = 0;
    };
}
