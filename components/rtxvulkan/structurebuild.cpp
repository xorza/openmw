#include "structurebuild.hpp"

#include <osg/Vec3f>

#include <components/rtx/meshrange.hpp>

namespace Rtx
{
    VkAccelerationStructureGeometryKHR describeTriangles(
        const MeshRange& mesh, const VkDeviceAddress positions, const VkDeviceAddress indices)
    {
        return VkAccelerationStructureGeometryKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry = { .triangles = {
                              .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                              .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                              .vertexData = { .deviceAddress = positions },
                              .vertexStride = sizeof(osg::Vec3f),
                              .maxVertex = mesh.mVertices.mCount > 0 ? mesh.mVertices.mCount - 1 : 0,
                              .indexType = VK_INDEX_TYPE_UINT32,
                              .indexData = { .deviceAddress = indices },
                          } },
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        };
    }

    void barrierBeforeBuild(const VkCommandBuffer commands)
    {
        const VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
                | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask
            = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask
            = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    void barrierAfterBuild(const VkCommandBuffer commands)
    {
        const VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }
}
