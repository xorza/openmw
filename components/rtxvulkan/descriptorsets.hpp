#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// `count` descriptor sets of one layout and the pool they came out of, which is what frees
    /// them: a pool per owner, sized from the layout's own bindings, so a binding the layout grew
    /// cannot leave the pool short. For the sets nothing pushes — a G-buffer's channels, a fog
    /// volume's two parities, a scene's bindless textures — which are written when what they name
    /// is made and bound as they are.
    class DescriptorSets
    {
    public:
        /// @param bindings what `layout` was made from, which is what the pool is sized by.
        /// @param flags what the pool needs beyond a plain one: update-after-bind for a set written
        ///        while a command that bound it is on the queue.
        DescriptorSets(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            VkDescriptorSetLayout layout, std::uint32_t count, VkDescriptorPoolCreateFlags flags = 0);

        VkDescriptorSet get(std::size_t index) const { return mSets[index]; }

    private:
        Owned<VkDescriptorPool, vkDestroyDescriptorPool> mPool;
        std::vector<VkDescriptorSet> mSets;
    };

    /// Writes `writes` into the sets they name, from the host and at once. Nothing where there are
    /// none, which Vulkan would take but a caller need not ask.
    void updateSets(const Device& device, std::span<const VkWriteDescriptorSet> writes);
}
