#include "spritebinpass.hpp"

#include <cassert>
#include <cstdint>

#include "device.hpp"
#include "gputimer.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t lanes, std::uint32_t workgroup)
        {
            return (lanes + workgroup - 1) / workgroup;
        }

        /// Orders one dispatch's writes against the next dispatch's reads and writes.
        void handOver(VkCommandBuffer commands, VkPipelineStageFlags2 from, VkAccessFlags2 wrote,
            VkPipelineStageFlags2 to, VkAccessFlags2 reads)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = from,
                .srcAccessMask = wrote,
                .dstStageMask = to,
                .dstAccessMask = reads,
            };
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(commands, &dependency);
        }

        void dispatch(VkCommandBuffer commands, const ComputePipeline& pipeline, const Shaders::SpriteBinConstants& bin,
            std::uint32_t groups)
        {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getHandle());
            vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bin), &bin);
            vkCmdDispatch(commands, groups, 1, 1);
        }
    }

    SpriteBinPass::SpriteBinPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mRects(device, {}, sizeof(Shaders::SpriteBinConstants), {}, shaderDirectory / "spriterects.comp.spv",
            "sprite rects")
        , mStarts(device, {}, sizeof(Shaders::SpriteBinConstants), {}, shaderDirectory / "spritestarts.comp.spv",
              "sprite starts")
        , mRuns(device, {}, sizeof(Shaders::SpriteBinConstants), {}, shaderDirectory / "spriteruns.comp.spv",
              "sprite runs")
    {
    }

    void SpriteBinPass::record(VkCommandBuffer commands, const Shaders::SpriteBinConstants& bin, const VkBuffer list,
        GpuTimer* const timer) const
    {
        assert(bin.mCamera.mWidth > 0 && bin.mCamera.mHeight > 0 && "a bin over a frame with no pixels");
        assert(bin.mSprites != 0 && bin.mEmitters != 0 && bin.mRects != 0 && bin.mList != 0 && bin.mReport != 0
            && "a bin over a table addressed as nothing");

        const std::uint32_t tiles
            = Shaders::spriteTilesOver(bin.mCamera.mWidth) * Shaders::spriteTilesOver(bin.mCamera.mHeight);

        openZone(timer, commands, "sprites");

        // **Behind everything the queue has done to this copy of the tables.** What last touched
        // them is the frame before last's bin and trace, or a picture's inside the interface, and
        // both are finished — the caller waited the fence — but a wait on the host is not a
        // dependency on the queue, and the layers say so: a fill two frames apart is a write after
        // a write with nothing ordering it. Sourced at every command, which is what the frame's
        // own first transitions already wait for, so this costs nothing the frame was not paying.
        handOver(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        // The counts start at nothing. The head is `tiles + 1` entries, and the fill is the
        // device's rather than a memset of the host's: it is the one cost that scales with the
        // tile count whatever the sprites do, and taking it off the host is what let the tile be
        // chosen for the trace.
        vkCmdFillBuffer(commands, list, 0, VkDeviceSize{ tiles + 1 } * sizeof(std::uint32_t), 0);
        handOver(commands, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        // A frame with no sprites has nothing to count and no run to fill, and the scan below is
        // what writes the starts it still has to have.
        if (bin.mCount > 0)
        {
            dispatch(commands, mRects, bin,
                groupsFor(bin.mCount * Shaders::SPRITE_BIN_LANES, Shaders::SPRITE_BIN_WORKGROUP));
            handOver(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }

        dispatch(commands, mStarts, bin, 1);

        // The runs read the starts, and the host reads the report after the fence — which a fence
        // alone does not make visible, so the host's read is named here.
        handOver(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);

        if (bin.mCount > 0)
            dispatch(
                commands, mRuns, bin, groupsFor(tiles * Shaders::SPRITE_RUNS_LANES, Shaders::SPRITE_RUNS_WORKGROUP));

        // The trace reads the list from its generation shader, and a picture's does the same.
        handOver(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        closeZone(timer, commands);
    }
}
