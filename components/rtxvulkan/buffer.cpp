#include "buffer.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
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

        mMemory = device.getMemory().take(requirements, properties, Tiling::Linear);
        checkVk(vkBindBufferMemory(device.getHandle(), mHandle.get(), mMemory.getHandle(), mMemory.getOffset()),
            "vkBindBufferMemory");

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
        return Buffer(device, size, usage, sHostWritten, false);
    }

    Buffer Buffer::staging(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage)
    {
        return Buffer(device, size, usage, sStaging, true);
    }

    Buffer growTo(Buffer& held, const Device& device, VkDeviceSize bytes, VkBufferUsageFlags usage)
    {
        // One byte and not none. Vulkan has no zero-sized buffer, so a table with nothing in it
        // still gets the smallest one that can be bound — which is what the shader's descriptor
        // needs and what nothing in it has to read.
        const VkDeviceSize wanted = std::max(bytes, VkDeviceSize{ 1 });
        if (held.getSize() >= wanted)
            return Buffer();

        Buffer displaced = std::move(held);
        held = Buffer::hostWritten(device, wanted, usage);

        return displaced;
    }

    void Buffer::orderForHostRead(VkCommandBuffer commands) const
    {
        assert(mReadable && "a host-read dependency on memory nothing reads back");

        const VkBufferMemoryBarrier2 written{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            // Every way this renderer fills one: a copy out of an image, and a shader writing
            // through the buffer's own address. Naming both here rather than at each of the three
            // callers is what stops one of them naming the wrong one.
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mHandle.get(),
            .size = VK_WHOLE_SIZE,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &written,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }
}
