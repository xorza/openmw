#include "fogtile.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include <components/rtx/fognoise.hpp>
#include <components/rtx/shaders/scene.h>

#include "commands.hpp"

namespace Rtx
{
    FogTile::FogTile(const Device& device, CommandPool& pool)
        : mField(device, Shaders::FOG_FIELD_SIZE, Shaders::FOG_FIELD_SIZE, VK_FORMAT_R8G8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "fog field", Shaders::FOG_FIELD_LEVELS,
            Shaders::FOG_FIELD_SIZE)
        , mSampler(Sampler::forContent(device, "fog field"))
    {
        const FogNoise noise = bakeFogNoise();

        // **Every level uploaded rather than halved from the one above.** A chain `buildMips` made
        // would be the mean of what is over it and nothing else, and this field's levels are each
        // stretched back to one spread — `bakeFogNoise` says why that is what a coverage band needs.
        // It could not make one for a volume in any case. Seventy-three kilobytes cross the bus once
        // for the life of the device.
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(Shaders::FOG_FIELD_LEVELS);
        for (std::uint32_t level = 0; level < Shaders::FOG_FIELD_LEVELS; ++level)
            regions.push_back(VkBufferImageCopy{
                .bufferOffset = noise.mOffsets[level],
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent = { mField.getWidthAt(level), mField.getHeightAt(level), mField.getDepthAt(level) },
            });

        Batch batch(pool);
        uploadImage(device, batch, mField, std::as_bytes(std::span(noise.mBytes)), regions);
        batch.flush();
    }
}
