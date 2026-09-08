#pragma once

#include <cstddef>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    struct MeshRange;

    // Addressable as well as build input, because the shader reads the indices back at a hit
    // through the same address the build read them at, and there is no reason for a second copy
    // of them to exist. No descriptor names any of these, so nothing else is asked for.
    inline constexpr VkBufferUsageFlags sBuildInputUsage
        = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    inline constexpr VkBufferUsageFlags sScratchUsage
        = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    /// One mesh's triangles as the builder takes them, out of addresses a caller worked out.
    ///
    /// **`maxVertex` is guarded, because a freed slot has no vertices.** A slot the scene has
    /// taken back keeps its index and its room and holds a count of zero until something fits
    /// into it; subtracting one there wraps, and the driver is handed four billion vertices.
    ///
    /// **Opaque as built**, and overridden per instance where a material says otherwise: opacity
    /// is a property of the material and a mesh does not carry one, so the top-level flags are
    /// the only place the question can be answered exactly.
    ///
    /// @param micromap what the mesh's cutout mask baked to, chained so that traversal resolves
    ///        each microtriangle the bake decided without the any-hit — or null for a mesh with
    ///        none, whose forced non-opaque rows reach the any-hit for every candidate.
    VkAccelerationStructureGeometryKHR describeTriangles(const MeshRange& mesh, VkDeviceAddress positions,
        VkDeviceAddress indices, const VkAccelerationStructureTrianglesOpacityMicromapEXT* micromap);

    /// Orders a build after the trace before it on the queue, which may still be reading what
    /// the build is about to write.
    ///
    /// **What the fence used to be.** With one frame in flight the trace had finished before the
    /// next placement was recorded; with two it has not, and a top level or a refit built over
    /// a structure a ray is walking is a torn structure. An execution dependency is all a
    /// write-after-read needs.
    void barrierBeforeBuild(VkCommandBuffer commands);

    /// Everything between a build and whatever reads the structure it wrote.
    void barrierAfterBuild(VkCommandBuffer commands);

    /// The scratch one run of structure builds is described in.
    ///
    /// **Members and not locals, because Vulkan keeps the addresses.** A build info holds
    /// `pGeometries` as a pointer and a range is handed over by address, so both have to outlive the
    /// loop that filled them — and a cell arriving must not allocate five vectors to say so.
    ///
    /// **Two passes, because `sizeTo` is what makes the first one possible.** A vector grown while a
    /// pointer already points into it moves its storage, so every geometry is placed before any
    /// build info names one. `BottomLevelStore::build` and `SceneAcceleration::prepareRefit` are
    /// each written that way, and this is where the rule is stated rather than in both of them.
    struct StructureBuildBatch
    {
        std::vector<VkAccelerationStructureGeometryKHR> mGeometries;

        /// What each geometry chains for its micromap, where it has one. Beside the geometries
        /// because the geometry keeps a pointer to it.
        std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> mMicromaps;

        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mBuilds;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> mRanges;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> mRangePointers;

        /// Room for `count` descriptions, each one cleared, and an empty list of range pointers.
        ///
        /// **Cleared and not merely sized**, because a filler may skip an entry — a mesh with no
        /// triangles is described by nobody — and what is left behind is then the previous run's.
        ///
        /// The pointers are pushed rather than sized, because how many there are is what a filler
        /// decides: every entry for a refit, and only what was really built otherwise.
        void sizeTo(std::size_t count)
        {
            mGeometries.assign(count, VkAccelerationStructureGeometryKHR{});
            mMicromaps.assign(count, VkAccelerationStructureTrianglesOpacityMicromapEXT{});
            mBuilds.assign(count, VkAccelerationStructureBuildGeometryInfoKHR{});
            mRanges.assign(count, VkAccelerationStructureBuildRangeInfoKHR{});

            mRangePointers.clear();
            mRangePointers.reserve(count);
        }
    };
}
