#include "buffer.hpp"

#include <algorithm>
#include <cassert>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// Video memory the host can write. Required rather than fallen back from — see `Requirements`.
        constexpr VkMemoryPropertyFlags sResizableBar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        constexpr VkMemoryPropertyFlags sStaging
            = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    Buffer::Buffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
        bool readable)
        : mSize(size)
        , mAddressable((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
        , mReadable(readable)
    {
        assert(size > 0);

        const VkBufferCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        checkVk(
            vkCreateBuffer(device.getHandle(), &create, nullptr, mHandle.put(device.getHandle())), "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device.getHandle(), mHandle.get(), &requirements);

        mMemory = DeviceMemory(device, requirements.size, requirements.memoryTypeBits, properties, mAddressable);
        checkVk(vkBindBufferMemory(device.getHandle(), mHandle.get(), mMemory.getHandle(), 0), "vkBindBufferMemory");

        if (mAddressable)
        {
            const VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = mHandle.get(),
            };
            mAddress = vkGetBufferDeviceAddress(device.getHandle(), &info);
        }
    }

    Buffer Buffer::deviceLocal(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage)
    {
        return Buffer(device, size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    }

    Buffer Buffer::hostWritten(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage)
    {
        return Buffer(device, size, usage, sResizableBar, false);
    }

    Buffer Buffer::staging(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage)
    {
        return Buffer(device, size, usage, sStaging, true);
    }

    Buffer growTo(Buffer& held, const Device& device, VkDeviceSize bytes, VkBufferUsageFlags usage)
    {
        // **One byte and not none.** Vulkan has no zero-sized buffer, so a table with nothing in it
        // still gets the smallest one that can be bound — which is what the shader's descriptor
        // needs and what nothing in it has to read.
        const VkDeviceSize wanted = std::max(bytes, VkDeviceSize{ 1 });
        if (held.getSize() >= wanted)
            return Buffer();

        Buffer displaced = std::move(held);
        held = Buffer::hostWritten(device, wanted, usage);

        return displaced;
    }
}
