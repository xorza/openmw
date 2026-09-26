#include "structurebuild.hpp"

#include <osg/Vec3f>

#include <components/rtx/mesh.hpp>

#include "barriers.hpp"
#include "imageuse.hpp"

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
            // **No duplicate candidate, or a see-through surface is counted twice.** The spec lets a
            // traversal report one triangle more than once unless the geometry says otherwise, and
            // `candidateStops` sums every report it gets.
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR | VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR,
        };
    }

    void barrierBeforeBuild(const VkCommandBuffer commands)
    {
        constexpr BufferUse built{ VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR };

        handOver(commands,
            BufferUse{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
                    | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                built.mAccess },
            built);
    }

    void barrierAfterBuild(const VkCommandBuffer commands)
    {
        handOver(commands,
            BufferUse{ VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR },
            BufferUse{ VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR });
    }
}
