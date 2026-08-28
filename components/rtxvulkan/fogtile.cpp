#include "fogtile.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include <components/rtx/fognoise.hpp>
#include <components/rtx/shaders/scene.h>

#include "buffer.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    FogTile::FogTile(const Device& device, CommandPool& pool)
        : mDevice(device)
        , mField(device, Shaders::FOG_FIELD_SIZE, Shaders::FOG_FIELD_SIZE, VK_FORMAT_R8G8_UNORM,
              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "fog field", Shaders::FOG_FIELD_LEVELS)
    {
        const FogNoise noise = bakeFogNoise();

        // **Every level uploaded rather than halved from the one above.** A chain `buildMips` made
        // would be the mean of what is over it and nothing else, and this field's levels are each
        // stretched back to one spread — `bakeFogNoise` says why that is what a coverage band needs.
        // A hundred and seventy kilobytes cross the bus once for the life of the device.
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(Shaders::FOG_FIELD_LEVELS);
        for (std::uint32_t level = 0; level < Shaders::FOG_FIELD_LEVELS; ++level)
            regions.push_back(VkBufferImageCopy{
                .bufferOffset = noise.mOffsets[level],
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent = { mField.getWidthAt(level), mField.getHeightAt(level), 1 },
            });

        Batch batch(pool);
        {
            Buffer staging(device, noise.mBytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            staging.write(std::span<const std::uint8_t>(noise.mBytes));

            const VkCommandBuffer commands = batch.getCommands();

            mField.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            vkCmdCopyBufferToImage(commands, staging.getHandle(), mField.getHandle(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());

            mField.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

            batch.keep(std::move(staging));
        }
        batch.flush();

        // After the image, for the reason `WavePass` gives: a member that throws while being
        // constructed leaves the ones already built to their own destructors, and a handle made in
        // this body would have none.
        const VkSamplerCreateInfo sampler{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        checkVk(vkCreateSampler(device.getHandle(), &sampler, nullptr, &mSampler), "vkCreateSampler");
    }

    FogTile::~FogTile()
    {
        if (mSampler != VK_NULL_HANDLE)
            vkDestroySampler(mDevice.getHandle(), mSampler, nullptr);
    }
}
