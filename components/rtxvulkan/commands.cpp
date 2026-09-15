#include "commands.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <exception>
#include <iterator>
#include <utility>

#include "barriers.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "image.hpp"
#include "memory.hpp"
#include "result.hpp"
#include "timeline.hpp"

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
        mHandle = Owned<VkCommandPool, vkDestroyCommandPool>::make(
            device.getHandle(), vkCreateCommandPool, create, "vkCreateCommandPool");
    }

    void CommandPool::reset()
    {
        assert(mDeferred.empty() && "a batch deferred to a submit that never came");

        checkVk(vkResetCommandPool(mDevice.getHandle(), mHandle.get(), VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT),
            "vkResetCommandPool");
    }

    void CommandPool::defer(VkCommandBuffer commands, std::vector<Buffer>&& staging)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        mDeferred.push_back(commands);

        std::move(staging.begin(), staging.end(), std::back_inserter(mDeferredStaging));
    }

    std::uint64_t CommandPool::submitWithDeferred(VkCommandBuffer commands,
        const std::span<const VkSemaphoreSubmitInfo> waits, const std::span<const VkSemaphoreSubmitInfo> signals)
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

        // The timeline's signal and then whatever the caller adds, which is a present's.
        Timeline& timeline = mDevice.getTimeline();
        const std::uint64_t value = timeline.next();
        mSignalScratch.clear();
        mSignalScratch.reserve(signals.size() + 1);
        mSignalScratch.push_back(timeline.signal(value));
        mSignalScratch.insert(mSignalScratch.end(), signals.begin(), signals.end());

        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size()),
            .pWaitSemaphoreInfos = waits.data(),
            .commandBufferInfoCount = static_cast<std::uint32_t>(mSubmitScratch.size()),
            .pCommandBufferInfos = mSubmitScratch.data(),
            .signalSemaphoreInfoCount = static_cast<std::uint32_t>(mSignalScratch.size()),
            .pSignalSemaphoreInfos = mSignalScratch.data(),
        };
        checkVk(mDevice, vkQueueSubmit2(mDevice.getQueue(), 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
        return value;
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

    std::uint64_t CommandPool::submit(VkCommandBuffer commands, Graveyard& kept,
        const std::span<const VkSemaphoreSubmitInfo> waits, const std::span<const VkSemaphoreSubmitInfo> signals)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

        // Buried before the value is taken, so the stamp is the value this submit signals: the
        // deferred batches run ahead of `commands` and are finished when it is.
        for (const VkCommandBuffer deferred : mDeferred)
            kept.bury(deferred);
        for (Buffer& staging : mDeferredStaging)
            kept.bury(std::move(staging));

        const std::uint64_t value = submitWithDeferred(commands, waits, signals);
        forgetDeferred();
        return value;
    }

    void CommandPool::discard(VkCommandBuffer commands)
    {
        // Neither ended nor submitted: a buffer still being recorded is not pending, so this is
        // where a recording nobody wants goes back.
        free(std::span<const VkCommandBuffer>(&commands, 1));
    }

    void CommandPool::free(std::span<const VkCommandBuffer> commands)
    {
        if (!commands.empty())
            vkFreeCommandBuffers(
                mDevice.getHandle(), mHandle.get(), static_cast<std::uint32_t>(commands.size()), commands.data());
    }

    std::vector<VkCommandBuffer> CommandPool::allocate(std::uint32_t count)
    {
        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = mHandle.get(),
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

        // **Everything earlier on the queue, before anything in this buffer.** A buffer is recorded
        // over a frame still running, and the barriers inside it are between its own passes; what
        // they do not say is that its first pass comes after that frame's last — a refit reading a
        // pose row an arrival's skin pass writes over, a copy into a table a trace reads through.
        // One full barrier at the head says it for every pass at once, whatever the pass, which is
        // what a dependency derived from each resource's state would say pass by pass and at far
        // greater length. A full barrier between passes already costs this renderer nothing it
        // could measure; a handful a frame at the seams cost the same — and every discard and
        // every first read of a copy in the buffer leans on it, sourcing itself at nothing.
        handOver(commands, Use::sBufferAnyReadWrite, Use::sBufferAnyReadWrite);
    }

    void CommandPool::end(VkCommandBuffer commands)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    }

    VkCommandBuffer CommandPool::begin()
    {
        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = mHandle.get(),
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

        mDevice.getTimeline().waitFor(submitWithDeferred(commands, {}, {}), "a one-off submit");

        // The copies have run, so this is where a deferred batch's staging stops being read, and
        // where every buffer that carried one can go back to the pool.
        free(mDeferred);
        free(std::span<const VkCommandBuffer>(&commands, 1));

        forgetDeferred();
    }

    Batch::~Batch()
    {
        if (mCommands != VK_NULL_HANDLE)
        {
            assert(
                std::uncaught_exceptions() > 0 && "a batch that recorded something was neither flushed nor deferred");

            mPool.discard(std::exchange(mCommands, VK_NULL_HANDLE));
        }

        release();
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

    StagingRun Batch::stage(std::span<const std::byte> bytes)
    {
        VkDeviceSize at = alignUp(mFilled, sStagingAlignment);

        if (mBlocks.empty() || at + bytes.size() > mBlocks.back().getSize())
        {
            mBlocks.push_back(Buffer::staging(getDevice(), std::max<VkDeviceSize>(bytes.size(), sStagingBlock),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "staging block"));
            at = 0;
        }

        const Buffer& block = mBlocks.back();
        block.writeAt(at, bytes);
        mFilled = at + bytes.size();

        return StagingRun{ .mBuffer = block.getHandle(), .mOffset = at };
    }

    void Batch::release()
    {
        mStaging.clear();
        mBlocks.clear();
        mFilled = 0;
    }

    void Batch::flush()
    {
        if (mCommands == VK_NULL_HANDLE)
        {
            // Staging with nothing recorded is a caller that kept a buffer and then decided against
            // the copy; it has no reader either way.
            release();
            return;
        }

        // Released before the wait can be skipped and after it cannot: the copies have run by the
        // time `endAndWait` returns, so this is where a staging buffer stops being read.
        mPool.endAndWait(std::exchange(mCommands, VK_NULL_HANDLE));
        release();
    }

    void Batch::defer()
    {
        if (mCommands == VK_NULL_HANDLE)
        {
            release();
            return;
        }

        // The blocks go with what callers handed over, because a deferred copy has not run: the
        // pool holds both until the submit that carries this batch has been waited on.
        mStaging.insert(
            mStaging.end(), std::make_move_iterator(mBlocks.begin()), std::make_move_iterator(mBlocks.end()));

        mPool.defer(std::exchange(mCommands, VK_NULL_HANDLE), std::move(mStaging));
        release();
    }

    void stageInto(Batch& batch, const Buffer& into, VkDeviceSize offset, std::span<const std::byte> bytes)
    {
        const StagingRun staged = batch.stage(bytes);
        const VkBufferCopy region{
            .srcOffset = staged.mOffset,
            .dstOffset = offset,
            .size = bytes.size(),
        };
        vkCmdCopyBuffer(batch.getCommands(), staged.mBuffer, into.getHandle(), 1, &region);

        // A copy takes a handle and names nothing on its own, and a host write over the destination
        // while the copy is still on the queue is a race between two writers: named, so `isIdle`
        // says so. The source needs no stamp: a staging block is the batch's, held until the
        // submit that carries it has been waited on.
        into.nameFor(batch.getDevice().getTimeline().getNext());
    }

    Buffer uploadBuffer(Batch& batch, std::span<const std::byte> bytes, VkBufferUsageFlags usage, std::string_view name)
    {
        const Device& device = batch.getDevice();

        // Host memory and not the aperture. These bytes are written once and read once by the
        // copy below, so putting them in the video memory the host writes into spends the scarcest
        // heap on a card without resizable BAR for a buffer that is gone by the next submit.
        Buffer staging = Buffer::staging(device, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "upload staging");
        staging.write(bytes);

        Buffer result = Buffer::deviceLocal(device, bytes.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, name);

        const VkCommandBuffer commands = batch.getCommands();
        staging.copyTo(commands, result, bytes.size());

        // What makes an upload self-contained. Batched, the next thing recorded may be an
        // acceleration structure built out of exactly these bytes, and without this it would read
        // them before the copy had run.
        result.transition(commands, Use::sBufferCopyWrite, Use::sBufferAnyRead);

        batch.keep(std::move(staging));

        return result;
    }

    void uploadImage(Batch& batch, Image& image, std::span<const std::byte> bytes, std::span<VkBufferImageCopy> regions)
    {
        const StagingRun staged = batch.stage(bytes);
        for (VkBufferImageCopy& region : regions)
            region.bufferOffset += staged.mOffset;

        const VkCommandBuffer commands = batch.getCommands();

        image.transition(commands, Use::sUndefined, Use::sCopyWrite);

        vkCmdCopyBufferToImage(commands, staged.mBuffer, image.getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<std::uint32_t>(regions.size()), regions.data());

        image.transition(commands, Use::sCopyWrite, Use::sTextureSample);
    }

    void orderStagedWrites(Batch& batch)
    {
        handOver(batch.getCommands(), Use::sBufferCopyWrite, Use::sBufferAnyRead);
    }
}
