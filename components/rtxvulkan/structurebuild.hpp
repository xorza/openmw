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
    /// `maxVertex` is guarded, because a freed slot holds a count of zero and subtracting one
    /// there hands the driver four billion vertices. Opaque as built and overridden per instance,
    /// because opacity is the material's and a mesh does not carry one.
    VkAccelerationStructureGeometryKHR describeTriangles(
        const MeshRange& mesh, VkDeviceAddress positions, VkDeviceAddress indices);

    /// Orders a build after the trace before it on the queue, which with two frames in flight may
    /// still be walking the structure the build is about to write. A barrier and not a fence,
    /// because an execution dependency is all a write-after-read needs.
    void barrierBeforeBuild(VkCommandBuffer commands);

    /// Everything between a build and whatever reads the structure it wrote.
    void barrierAfterBuild(VkCommandBuffer commands);

    /// The scratch one run of structure builds is described in. Members and not locals, because a
    /// build info holds `pGeometries` by pointer and a cell arriving must not allocate four vectors.
    /// Every geometry is placed by `sizeTo` before any build info names one, because a vector
    /// grown while a pointer points into it moves its storage.
    struct StructureBuildBatch
    {
        std::vector<VkAccelerationStructureGeometryKHR> mGeometries;

        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mBuilds;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> mRanges;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> mRangePointers;

        /// Room for `count` descriptions, each one cleared because a filler may skip a mesh with
        /// no triangles, and an empty list of range pointers, because how many there are is what
        /// the filler decides.
        void sizeTo(std::size_t count)
        {
            mGeometries.assign(count, VkAccelerationStructureGeometryKHR{});
            mBuilds.assign(count, VkAccelerationStructureBuildGeometryInfoKHR{});
            mRanges.assign(count, VkAccelerationStructureBuildRangeInfoKHR{});

            mRangePointers.clear();
            mRangePointers.reserve(count);
        }
    };
}
