#include "skinpass.hpp"

#include <cassert>

#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/skinning.h>

#include "device.hpp"
#include "gputimer.hpp"
#include "skintables.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t vertices)
        {
            return (vertices + Shaders::SKIN_WORKGROUP - 1) / Shaders::SKIN_WORKGROUP;
        }

        /// Orders the dispatches just recorded against everything that reads what they wrote: the
        /// refit, which reads the positions as build input, and the trace, which reads the normals.
        void handOver(VkCommandBuffer commands)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                    | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            };
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(commands, &dependency);
        }
    }

    SkinPass::SkinPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mSkin(device, {}, sizeof(Shaders::SkinConstants), {}, shaderDirectory / "skin.comp.spv", "skin")
        , mMorph(device, {}, sizeof(Shaders::MorphConstants), {}, shaderDirectory / "morph.comp.spv", "morph")
    {
    }

    bool SkinPass::record(VkCommandBuffer commands, const SceneDesc& scene, const std::uint32_t slot,
        SkinTables& tables, SlotBlocks& positions, SlotBlocks& normals, GpuTimer* const timer) const
    {
        // **Owed to every copy, and paid to this one.** A mesh that moved this frame reaches this
        // copy now and the other on the frame after next; a mesh that moved last frame and stands
        // still now is still owed here, or this copy would carry a pose two frames old the next
        // time it was traced.
        positions.write(scene.getDeformed());

        // One pipeline bound at a time, and a bind only where the kind changes: a crowd is one
        // kind for most of its length.
        const ComputePipeline* bound = nullptr;
        bool recorded = false;

        BlockedBuffer& normalsInto = normals.at(slot);
        positions.sync(slot, [&](const Index index, BlockedBuffer& into) {
            const MeshRange& mesh = scene.getMeshes()[index];

            // A slot owed from before it went, or one taken over by a mesh that stands: nothing
            // to pose. Its run in the positions holds what the arrival wrote.
            if (mesh.mDeform == Deform::None || mesh.mVertexCount == 0)
                return;

            if (!recorded)
            {
                openZone(timer, commands, "skin");
                recorded = true;
            }

            const VkDeviceAddress posed = into.addressOf(mesh.mVertexOffset);
            const VkDeviceAddress shaded = normalsInto.addressOf(mesh.mVertexOffset);

            if (mesh.mDeform == Deform::Rig)
            {
                const Rig& rig = scene.getRigs()[mesh.mDeformer];
                const Shaders::SkinConstants push{
                    .mBindPositions = tables.getBindPositions(mesh),
                    .mBindNormals = tables.getBindNormals(mesh),
                    .mRuns = tables.getRuns(rig),
                    .mInfluences = tables.getInfluences(rig),
                    .mBones = tables.writeBones(scene, slot, index),
                    .mPositions = posed,
                    .mNormals = shaded,
                    .mCount = mesh.mVertexCount,
                    .mPadding = 0,
                };

                if (bound != &mSkin)
                {
                    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mSkin.getHandle());
                    bound = &mSkin;
                }

                vkCmdPushConstants(commands, mSkin.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            }
            else
            {
                const Morph& morph = scene.getMorphs()[mesh.mDeformer];
                const Shaders::MorphConstants push{
                    .mBase = tables.getBindPositions(mesh),
                    .mOffsets = tables.getMorphOffsets(morph),
                    .mWeights = tables.writeWeights(scene, slot, index),
                    .mPositions = posed,
                    .mCount = mesh.mVertexCount,
                    .mTargets = morph.mTargetCount,
                };

                if (bound != &mMorph)
                {
                    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mMorph.getHandle());
                    bound = &mMorph;
                }

                vkCmdPushConstants(commands, mMorph.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            }

            vkCmdDispatch(commands, groupsFor(mesh.mVertexCount), 1, 1);
        });

        if (!recorded)
            return false;

        handOver(commands);
        closeZone(timer, commands);
        return true;
    }
}
