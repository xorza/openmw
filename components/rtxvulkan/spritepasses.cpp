#include "spritepasses.hpp"

#include <cassert>
#include <cstdint>

#include "buffer.hpp"
#include "device.hpp"
#include "dispatch.hpp"
#include "gputimer.hpp"

namespace Rtx
{
    namespace
    {
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

    void SpriteBinPass::record(VkCommandBuffer commands, const Shaders::SpriteBinConstants& bin, const Buffer& list,
        GpuTimer* const timer) const
    {
        assert(bin.mCamera.mWidth > 0 && bin.mCamera.mHeight > 0 && "a bin over a frame with no pixels");
        assert(bin.mSprites != 0 && bin.mEmitters != 0 && bin.mRects != 0 && bin.mList != 0 && bin.mReport != 0
            && "a bin over a table addressed as nothing");

        const std::uint32_t tiles = Shaders::spriteTilesIn(bin.mCamera.mWidth, bin.mCamera.mHeight);

        // The one place the list's length and the capacity the shader is told meet: a buffer
        // shorter than their sum is three dispatches writing past the end of it, where a capacity
        // smaller than the runs need is only a slow frame.
        assert(list.getSize() >= (VkDeviceSize{ tiles } + 1 + bin.mCapacity) * sizeof(std::uint32_t)
            && "a sprite list shorter than its starts and its capacity together");

        openZone(timer, commands, "sprites");

        // Behind everything the queue did to this copy of the tables: the caller waited the fence,
        // but a wait on the host is not a dependency on the queue, and the layers flag a fill two
        // frames apart as an unordered write after a write. Costs nothing the frame's own first
        // transitions were not paying.
        handOver(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        // The counts start at nothing. The head is `tiles + 1` entries, and the fill is the
        // device's rather than a memset of the host's: it is the one cost that scales with the
        // tile count whatever the sprites do, and taking it off the host is what let the tile be
        // chosen for the trace.
        vkCmdFillBuffer(commands, list.getHandle(), 0, (VkDeviceSize{ tiles } + 1) * sizeof(std::uint32_t), 0);
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

    SpriteShadePass::SpriteShadePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mShade(device, {}, sizeof(Shaders::SpriteShadeConstants), {}, shaderDirectory / "spriteshade.comp.spv",
            "sprite shade")
    {
    }

    void SpriteShadePass::record(
        VkCommandBuffer commands, const Shaders::SpriteShadeConstants& shade, GpuTimer* const timer) const
    {
        assert(shade.mSprites != 0 && shade.mEmitters != 0 && shade.mOrder != 0
            && "a shading over a table addressed as nothing");

        // A frame with no emitters has nothing to shade, and the sprites it has none of carry the
        // nought they were built with.
        if (shade.mEmitterCount == 0 || shade.mCount == 0)
            return;

        openZone(timer, commands, "shade");

        // **Behind the write that put the sprites there**, which is the host's, and behind whatever
        // the queue last did to this copy — the frame before last's trace. The caller waited that
        // frame's fence, but a wait on the host is not a dependency on the queue.
        handOver(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mShade.getHandle());
        vkCmdPushConstants(commands, mShade.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shade), &shade);

        // **One workgroup per emitter per light, and the ones with nothing to do exit at once.** The
        // count is tens, so a workgroup that reads its emitter and returns is cheaper than a host
        // pass that worked out which ones to launch and wrote a list of them.
        vkCmdDispatch(commands, shade.mEmitterCount * Shaders::SPRITE_SHADE_LIGHTS, 1, 1);

        // The bin reads the sprites next and the trace reads them after that.
        handOver(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        closeZone(timer, commands);
    }
}
