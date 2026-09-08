#include "skinpass.hpp"

#include <cassert>

#include <components/rtx/scenetables.hpp>
#include <components/rtx/shaders/skinning.h>

#include "device.hpp"
#include "dispatch.hpp"
#include "gputimer.hpp"
#include "skintables.hpp"

namespace Rtx
{
    namespace
    {
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

    bool SkinPass::record(VkCommandBuffer commands, const SceneTables& scene, const FrameSlot slot, SkinTables& tables,
        SlotBlocks& poses, SlotBlocks& normals, GpuTimer* const timer) const
    {
        // **Owed to every copy, and paid to this one.** A mesh that moved this frame reaches this
        // copy now and the other on the frame after next; a mesh that moved last frame and stands
        // still now is still owed here, or this copy would carry a pose two frames old the next
        // time it was traced.
        poses.write(scene.mMeshes.getDeformed());

        // One pipeline bound at a time, and a bind only where the kind changes: a crowd is one
        // kind for most of its length.
        const ComputePipeline* bound = nullptr;
        bool recorded = false;

        BlockedBuffer& normalsInto = normals.at(slot);
        poses.sync(slot, [&](const Index index, BlockedBuffer& into) {
            const MeshRange& mesh = scene.mMeshes.getRows()[index];

            // A slot owed from before it went, or one taken over by a mesh that stands: nothing
            // to pose. Its run in the poses holds what the arrival wrote.
            if (mesh.mDeform == Deform::None || mesh.mVertices.empty())
                return;

            if (!recorded)
            {
                openZone(timer, commands, "skin");
                recorded = true;
            }

            // **The pose table is indexed by the bind offset and the normals by the scene's own.**
            // A hit reads a normal out of the shared table, so every mesh has a run there; nothing
            // reads a position at a hit, so only the bodies have one here.
            const VkDeviceAddress posed = into.addressOf(mesh.mBindOffset);
            const VkDeviceAddress shaded = normalsInto.addressOf(mesh.mVertices.mOffset);

            if (mesh.mDeform == Deform::Rig)
            {
                const Rig& rig = scene.mDeformers.getRigs()[mesh.mDeformer];
                const Shaders::SkinConstants push{
                    .mBindPositions = tables.getBindPositions(mesh),
                    .mBindNormals = tables.getBindNormals(mesh),
                    .mRuns = tables.getRuns(rig),
                    .mInfluences = tables.getInfluences(rig),
                    .mBones = tables.writeBones(scene, slot, index),
                    .mPositions = posed,
                    .mNormals = shaded,
                    .mCount = mesh.mVertices.mCount,
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
                const Morph& morph = scene.mDeformers.getMorphs()[mesh.mDeformer];
                const Shaders::MorphConstants push{
                    .mBase = tables.getBindPositions(mesh),
                    .mOffsets = tables.getMorphOffsets(morph),
                    .mWeights = tables.writeWeights(scene, slot, index),
                    .mPositions = posed,
                    .mCount = mesh.mVertices.mCount,
                    .mTargets = morph.mTargetCount,
                };

                if (bound != &mMorph)
                {
                    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mMorph.getHandle());
                    bound = &mMorph;
                }

                vkCmdPushConstants(commands, mMorph.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            }

            vkCmdDispatch(commands, groupsFor(mesh.mVertices.mCount, Shaders::SKIN_WORKGROUP), 1, 1);
        });

        if (!recorded)
            return false;

        handOver(commands);
        closeZone(timer, commands);
        return true;
    }
}
