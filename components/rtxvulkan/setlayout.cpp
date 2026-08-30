#include "setlayout.hpp"

#include <cstdint>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    SetLayout::SetLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        VkDescriptorSetLayoutCreateFlags flags, const void* next)
    {
        const VkDescriptorSetLayoutCreateInfo describe{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = next,
            .flags = flags,
            .bindingCount = static_cast<std::uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };
        checkVk(vkCreateDescriptorSetLayout(device.getHandle(), &describe, nullptr, mHandle.put(device.getHandle())),
            "vkCreateDescriptorSetLayout");
    }
}
