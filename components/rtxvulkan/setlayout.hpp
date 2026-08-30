#pragma once

#include <span>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    class Device;

    /// A descriptor set layout, and the handle's lifetime.
    ///
    /// **The one Vulkan handle in this renderer that had no type of its own.** `Buffer`, `Image`,
    /// `DeviceMemory` and `PipelineCache` are each a create, a destroy and a getter behind an object
    /// that cannot leak one; a set layout was five copies of the same three lines instead. What each
    /// layout *is* stays with the thing that knows — `GBuffer::describeLayout` and its siblings build
    /// the binding list and hand back one of these.
    ///
    /// @param flags what a push descriptor set needs and a bound one does not.
    /// @param next binding flags, where the set is bindless. Read here and never kept.
    class SetLayout
    {
    public:
        SetLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            VkDescriptorSetLayoutCreateFlags flags = 0, const void* next = nullptr);
        ~SetLayout();

        SetLayout(const SetLayout&) = delete;
        SetLayout& operator=(const SetLayout&) = delete;

        VkDescriptorSetLayout getHandle() const { return mHandle; }

    private:
        const Device& mDevice;
        VkDescriptorSetLayout mHandle = VK_NULL_HANDLE;
    };
}
