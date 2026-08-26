#include "commands.hpp"

#include <cassert>
#include <exception>
#include <utility>

#include <components/debug/debuglog.hpp>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    CommandPool::CommandPool(const Device& device)
        : mDevice(device)
    {
        const VkCommandPoolCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = device.getQueueFamily(),
        };
        checkVk(vkCreateCommandPool(device.getHandle(), &create, nullptr, &mHandle), "vkCreateCommandPool");

        const VkFenceCreateInfo fence{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        checkVk(vkCreateFence(device.getHandle(), &fence, nullptr, &mFence), "vkCreateFence");
    }

    CommandPool::~CommandPool()
    {
        if (mFence != VK_NULL_HANDLE)
            vkDestroyFence(mDevice.getHandle(), mFence, nullptr);
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyCommandPool(mDevice.getHandle(), mHandle, nullptr);
    }

    void CommandPool::reset()
    {
        checkVk(vkResetCommandPool(mDevice.getHandle(), mHandle, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT),
            "vkResetCommandPool");
    }

    std::vector<VkCommandBuffer> CommandPool::allocate(std::uint32_t count)
    {
        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = mHandle,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = count,
        };

        std::vector<VkCommandBuffer> buffers(count);
        checkVk(vkAllocateCommandBuffers(mDevice.getHandle(), &allocate, buffers.data()), "vkAllocateCommandBuffers");
        return buffers;
    }

    VkCommandBuffer CommandPool::begin()
    {
        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = mHandle,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        VkCommandBuffer commands = VK_NULL_HANDLE;
        checkVk(vkAllocateCommandBuffers(mDevice.getHandle(), &allocate, &commands), "vkAllocateCommandBuffers");

        const VkCommandBufferBeginInfo begin{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        checkVk(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");

        return commands;
    }

    void CommandPool::endAndWait(VkCommandBuffer commands)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

        const VkCommandBufferSubmitInfo buffer{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = commands,
        };
        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &buffer,
        };

        checkVk(vkResetFences(mDevice.getHandle(), 1, &mFence), "vkResetFences");
        checkVk(vkQueueSubmit2(mDevice.getQueue(), 1, &submit, mFence), "vkQueueSubmit2");
        awaitVk(mDevice.getHandle(), mFence, "a one-off submit");

        vkFreeCommandBuffers(mDevice.getHandle(), mHandle, 1, &commands);
    }

    Batch::~Batch()
    {
        try
        {
            flush();
        }
        catch (const std::exception& e)
        {
            // A submit fails when the device is lost, and terminating out of a destructor tells
            // nobody which one it was.
            Log(Debug::Error) << "a command batch could not be submitted: " << e.what();
        }
    }

    VkCommandBuffer Batch::getCommands()
    {
        if (mCommands == VK_NULL_HANDLE)
            mCommands = mPool.begin();

        return mCommands;
    }

    void Batch::keep(Buffer&& staging)
    {
        mStaging.push_back(std::move(staging));
    }

    void Batch::keep(HostBuffer&& staging)
    {
        mHostStaging.push_back(std::move(staging));
    }

    void Batch::flush()
    {
        if (mCommands == VK_NULL_HANDLE)
        {
            // Staging with nothing recorded is a caller that kept a buffer and then decided against
            // the copy; it has no reader either way.
            mStaging.clear();
            mHostStaging.clear();
            return;
        }

        // Cleared before the wait can be skipped and after it cannot: the copies have run by the
        // time `endAndWait` returns, so this is where a staging buffer stops being read.
        mPool.endAndWait(std::exchange(mCommands, VK_NULL_HANDLE));
        mStaging.clear();
        mHostStaging.clear();
    }

    Buffer uploadBuffer(const Device& device, Batch& batch, std::span<const std::byte> bytes, VkBufferUsageFlags usage)
    {
        Buffer staging(device, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.write(bytes);

        Buffer result(
            device, bytes.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        const VkCommandBuffer commands = batch.getCommands();
        const VkBufferCopy region{ .size = bytes.size() };
        vkCmdCopyBuffer(commands, staging.getHandle(), result.getHandle(), 1, &region);

        // **What makes an upload self-contained.** Batched, the next thing recorded may be an
        // acceleration structure built out of exactly these bytes, and without this it would read
        // them before the copy had run.
        const VkBufferMemoryBarrier2 copied{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = result.getHandle(),
            .size = VK_WHOLE_SIZE,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &copied,
        };
        vkCmdPipelineBarrier2(commands, &dependency);

        batch.keep(std::move(staging));

        return result;
    }
}
