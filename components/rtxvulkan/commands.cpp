#include "commands.hpp"

#include <cassert>
#include <cstdint>
#include <iterator>
#include <utility>

#include "device.hpp"
#include "graveyard.hpp"
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
        assert(mDeferred.empty() && "a batch deferred to a submit that never came");

        checkVk(vkResetCommandPool(mDevice.getHandle(), mHandle, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT),
            "vkResetCommandPool");
    }

    void CommandPool::defer(VkCommandBuffer commands, std::vector<Buffer>&& staging)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        mDeferred.push_back(commands);

        std::move(staging.begin(), staging.end(), std::back_inserter(mDeferredStaging));
    }

    void CommandPool::submitWithDeferred(VkCommandBuffer commands, VkFence fence)
    {
        mSubmitScratch.clear();
        mSubmitScratch.reserve(mDeferred.size() + 1);
        for (const VkCommandBuffer deferred : mDeferred)
            mSubmitScratch.push_back(VkCommandBufferSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = deferred,
            });
        mSubmitScratch.push_back(VkCommandBufferSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = commands,
        });

        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = static_cast<std::uint32_t>(mSubmitScratch.size()),
            .pCommandBufferInfos = mSubmitScratch.data(),
        };

        if (fence != VK_NULL_HANDLE)
            checkVk(vkResetFences(mDevice.getHandle(), 1, &fence), "vkResetFences");
        checkVk(mDevice, vkQueueSubmit2(mDevice.getQueue(), 1, &submit, fence), "vkQueueSubmit2");
    }

    void CommandPool::forgetDeferred()
    {
        mDeferred.clear();
        mDeferredStaging.clear();
    }

    void CommandPool::finishDeferred()
    {
        if (mDeferred.empty())
            return;

        submitAndWait([](VkCommandBuffer) {});
    }

    void CommandPool::submit(VkCommandBuffer commands, VkFence fence, Graveyard& kept)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

        submitWithDeferred(commands, fence);

        // The deferred batches run ahead of `commands` and are finished when it is, so what they
        // hold goes under the same fence.
        for (const VkCommandBuffer deferred : mDeferred)
            kept.bury(deferred);
        for (Buffer& staging : mDeferredStaging)
            kept.bury(std::move(staging));

        forgetDeferred();
    }

    void CommandPool::free(std::span<const VkCommandBuffer> commands)
    {
        if (!commands.empty())
            vkFreeCommandBuffers(
                mDevice.getHandle(), mHandle, static_cast<std::uint32_t>(commands.size()), commands.data());
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

    void CommandPool::begin(VkCommandBuffer commands)
    {
        const VkCommandBufferBeginInfo begin{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        checkVk(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
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
        begin(commands);
        return commands;
    }

    void CommandPool::endAndWait(VkCommandBuffer commands)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

        submitWithDeferred(commands, mFence);
        awaitVk(mDevice, mFence, "a one-off submit");

        // The copies have run, so this is where a deferred batch's staging stops being read, and
        // where every buffer that carried one can go back to the pool.
        free(mDeferred);
        free(std::span<const VkCommandBuffer>(&commands, 1));

        forgetDeferred();
    }

    Batch::~Batch()
    {
        // A submit fails when the device is lost, and terminating out of a destructor tells nobody
        // which one it was. `tearDown` is where that rule lives.
        tearDown("a command batch could not be submitted", [&] { flush(); });
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

    void Batch::flush()
    {
        if (mCommands == VK_NULL_HANDLE)
        {
            // Staging with nothing recorded is a caller that kept a buffer and then decided against
            // the copy; it has no reader either way.
            mStaging.clear();
            return;
        }

        // Cleared before the wait can be skipped and after it cannot: the copies have run by the
        // time `endAndWait` returns, so this is where a staging buffer stops being read.
        mPool.endAndWait(std::exchange(mCommands, VK_NULL_HANDLE));
        mStaging.clear();
    }

    void Batch::defer()
    {
        if (mCommands == VK_NULL_HANDLE)
        {
            mStaging.clear();
            return;
        }

        mPool.defer(std::exchange(mCommands, VK_NULL_HANDLE), std::move(mStaging));
        mStaging.clear();
    }

    Buffer uploadBuffer(const Device& device, Batch& batch, std::span<const std::byte> bytes, VkBufferUsageFlags usage)
    {
        Buffer staging = Buffer::hostWritten(device, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        staging.write(bytes);

        Buffer result = Buffer::deviceLocal(device, bytes.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

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
