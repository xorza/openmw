#include "skinpass.hpp"

#include <cassert>

#include <components/rtx/deformertable.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/skinning.h>

#include "barriers.hpp"
#include "blockedbuffer.hpp"
#include "device.hpp"
#include "dispatch.hpp"
#include "frameslots.hpp"
#include "gputimer.hpp"
#include "imageuse.hpp"
#include "pipeline.hpp"
#include "skintables.hpp"

namespace Rtx
{
    namespace
    {
        /// Whether a mesh has a pose to compute. A slot owed from before it went, or one taken over
        /// by a mesh that stands, has nothing: its run in the poses holds what the arrival wrote.
        bool posable(const MeshRange& mesh)
        {
            return mesh.mDeform != Deform::None && !mesh.mVertices.empty();
        }

        /// Orders the dispatches just recorded against everything that reads what they wrote: the
        /// refit, which reads the positions as build input, and the trace, which reads the normals.
        void posed(VkCommandBuffer commands)
        {
            handOver(commands, Use::sBufferComputeWrite,
                BufferUse{ VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                        | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT });
        }
    }

    SkinPass::SkinPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mSkin(device, {}, sizeof(Shaders::SkinConstants), {}, shaderDirectory / "skin.comp.spv", "skin")
        , mMorph(device, {}, sizeof(Shaders::MorphConstants), {}, shaderDirectory / "morph.comp.spv", "morph")
    {
    }

    void SkinPass::pose(VkCommandBuffer commands, const SceneDesc& scene, const FrameSlot slot, const Index index,
        SkinTables& tables, const Rows rows, BlockedBuffer& into, BlockedBuffer& normalsInto,
        const ComputePipeline*& bound) const
    {
        const MeshRange& mesh = scene.meshes().getRows()[index];
        assert(posable(mesh) && "a pose of a mesh with nothing to pose");

        // The pose table is indexed by the bind offset and the normals by the scene's own. A hit
        // reads a normal out of the shared table, so every mesh has a run there; nothing reads a
        // position at a hit, so only the bodies have one here.
        const VkDeviceAddress posed = into.addressOf(mesh.mBindOffset);
        const VkDeviceAddress shaded = normalsInto.addressOf(mesh.mVertices.mOffset);

        // The pose, whichever kind: the bones as rows or the weights four to a word, as
        // `Rtx::PoseWord` lays them, at the one address either kernel reads from nought.
        const VkDeviceAddress pose
            = rows == Rows::Written ? tables.writePose(scene, slot, index) : tables.getPose(mesh, slot);

        const Deformer& deformer = scene.deformers().getDeformers()[mesh.mDeformer];
        if (deformer.mKind == Deform::Rig)
        {
            const Shaders::SkinConstants push{
                .mBindPositions = tables.getBindPositions(mesh),
                .mBindNormals = tables.getBindNormals(mesh),
                .mRuns = tables.getRuns(deformer),
                .mInfluences = tables.getInfluences(deformer),
                .mBones = pose,
                .mPositions = posed,
                .mNormals = shaded,
                .mCount = mesh.mVertices.mCount,
                .mPadding = 0,
            };

            if (bound != &mSkin)
            {
                bind(commands, mSkin);
                bound = &mSkin;
            }

            pushConstants(commands, mSkin, push);
        }
        else
        {
            const Shaders::MorphConstants push{
                .mBase = tables.getBindPositions(mesh),
                .mOffsets = tables.getMorphOffsets(deformer),
                .mWeights = pose,
                .mPositions = posed,
                .mCount = mesh.mVertices.mCount,
                .mTargets = deformer.mRows,
            };

            if (bound != &mMorph)
            {
                bind(commands, mMorph);
                bound = &mMorph;
            }

            pushConstants(commands, mMorph, push);
        }

        vkCmdDispatch(commands, groupsFor(mesh.mVertices.mCount, Shaders::SKIN_WORKGROUP), 1, 1);
    }

    bool SkinPass::record(VkCommandBuffer commands, const SceneDesc& scene, const FrameSlot slot, SkinTables& tables,
        SlotBlocks& poses, SlotBlocks& normals, GpuTimer* const timer) const
    {
        // Owed to every copy, and paid to this one: a mesh that moved last frame and stands still
        // now is still owed here, or this copy would carry a pose two frames old.
        poses.write(scene.meshes().getDeformed());

        // One pipeline bound at a time, and a bind only where the kind changes: a crowd is one
        // kind for most of its length.
        const ComputePipeline* bound = nullptr;
        bool recorded = false;

        BlockedBuffer& normalsInto = normals.at(slot);
        poses.sync(slot, [&](const Index index, BlockedBuffer& into) {
            if (!posable(scene.meshes().getRows()[index]))
                return;

            if (!recorded)
            {
                openZone(timer, commands, "skin");
                recorded = true;
            }

            pose(commands, scene, slot, index, tables, Rows::Written, into, normalsInto, bound);
        });

        if (!recorded)
            return false;

        posed(commands);
        closeZone(timer, commands);
        return true;
    }

    bool SkinPass::recordArrived(VkCommandBuffer commands, const SceneDesc& scene, const FrameSlot slot,
        const std::span<const Index> arrived, SkinTables& tables, SlotBlocks& poses, SlotBlocks& normals) const
    {
        const ComputePipeline* bound = nullptr;
        bool recorded = false;

        BlockedBuffer& into = poses.at(slot);
        BlockedBuffer& normalsInto = normals.at(slot);
        for (const Index index : arrived)
        {
            if (!posable(scene.meshes().getRows()[index]))
                continue;

            pose(commands, scene, slot, index, tables, Rows::Staged, into, normalsInto, bound);
            recorded = true;
        }

        if (!recorded)
            return false;

        posed(commands);
        return true;
    }
}
