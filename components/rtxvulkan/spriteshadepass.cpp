#include "spriteshadepass.hpp"

#include <cassert>

#include "device.hpp"
#include "dispatch.hpp"
#include "gputimer.hpp"

namespace Rtx
{
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
