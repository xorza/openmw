#include "sampler.hpp"

#include <cstdint>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        Owned<VkSampler, vkDestroySampler> create(
            const Device& device, const VkSamplerCreateInfo& describe, std::string_view name)
        {
            Owned<VkSampler, vkDestroySampler> handle;
            checkVk(vkCreateSampler(device.getHandle(), &describe, nullptr, handle.put(device.getHandle())),
                "vkCreateSampler");
            device.setName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<std::uint64_t>(handle.get()), name);
            return handle;
        }
    }

    Sampler Sampler::forTarget(const Device& device, std::string_view name)
    {
        const VkSamplerCreateInfo describe{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = VK_LOD_CLAMP_NONE,
        };

        return Sampler(create(device, describe, name));
    }

    Sampler Sampler::forContent(const Device& device, std::string_view name)
    {
        const VkSamplerCreateInfo describe{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            // Off, and not an oversight: every fetch names its own level, and anisotropic filtering
            // only applies to the implicit and gradient forms. A cone is isotropic by construction.
            .anisotropyEnable = VK_FALSE,
            .maxLod = VK_LOD_CLAMP_NONE,
        };

        return Sampler(create(device, describe, name));
    }
}
