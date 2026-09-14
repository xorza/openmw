#include "descriptorsets.hpp"

#include <algorithm>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    DescriptorSets::DescriptorSets(const Device& device, const std::span<const VkDescriptorSetLayoutBinding> bindings,
        const VkDescriptorSetLayout layout, const std::uint32_t count, const VkDescriptorPoolCreateFlags flags)
    {
        // One size per descriptor type the bindings use, summed over the bindings and the sets:
        // what the layout declares is what the pool has to hold, and nothing else.
        std::vector<VkDescriptorPoolSize> sizes;
        for (const VkDescriptorSetLayoutBinding& binding : bindings)
        {
            const auto same = [&](const VkDescriptorPoolSize& size) { return size.type == binding.descriptorType; };
            const auto found = std::find_if(sizes.begin(), sizes.end(), same);
            if (found != sizes.end())
                found->descriptorCount += binding.descriptorCount * count;
            else
                sizes.push_back(VkDescriptorPoolSize{ binding.descriptorType, binding.descriptorCount * count });
        }

        const VkDescriptorPoolCreateInfo describePool{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = flags,
            .maxSets = count,
            .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
            .pPoolSizes = sizes.data(),
        };
        mPool = Owned<VkDescriptorPool, vkDestroyDescriptorPool>::make(
            device.getHandle(), vkCreateDescriptorPool, describePool, "vkCreateDescriptorPool");

        const std::vector<VkDescriptorSetLayout> layouts(count, layout);
        const VkDescriptorSetAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = mPool.get(),
            .descriptorSetCount = count,
            .pSetLayouts = layouts.data(),
        };
        mSets.resize(count);
        checkVk(vkAllocateDescriptorSets(device.getHandle(), &allocate, mSets.data()), "vkAllocateDescriptorSets");
    }

    void updateSets(const Device& device, const std::span<const VkWriteDescriptorSet> writes)
    {
        if (writes.empty())
            return;

        vkUpdateDescriptorSets(device.getHandle(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}
