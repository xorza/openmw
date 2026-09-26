#include "commands.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

#include "barriers.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "image.hpp"
#include "imageuse.hpp"
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

    void CommandPool::defer(VkCommandBuffer commands)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        mDeferred.push_back(commands);
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

        // Which frame the driver's pacing is to count this submit under, where it paces: every
        // submit between one present and the next is that next present's.
        const VkLatencySubmissionPresentIdNV presented{
            .sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV,
            .presentID = mPresentId,
        };

        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = mPresentId != 0 ? &presented : nullptr,
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

    void CommandPool::finishDeferred()
    {
        if (mDeferred.empty())
            return;

        submitAndWait([](VkCommandBuffer) {});
    }

    std::uint64_t CommandPool::submit(VkCommandBuffer commands, const std::span<const VkSemaphoreSubmitInfo> waits,
        const std::span<const VkSemaphoreSubmitInfo> signals)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

        const std::uint64_t value = submitWithDeferred(commands, waits, signals);

        // Under the value this submit signals: the deferred batches run ahead of `commands` and
        // are finished when it is.
        for (VkCommandBuffer deferred : mDeferred)
            mRetiring.hold(value, std::move(deferred));
        mDeferred.clear();

        return value;
    }

    void CommandPool::collect()
    {
        releaseRetired(mDevice.getTimeline().getKnownFinished());
    }

    void CommandPool::collectIdle()
    {
        assert(mDevice.getTimeline().isIdle() && "command buffers given back under a submit still on the queue");
        assert(mDeferred.empty() && "command buffers given back under a batch not yet submitted");

        releaseRetired(std::numeric_limits<std::uint64_t>::max());
    }

    void CommandPool::releaseRetired(const std::uint64_t finished)
    {
        mRetiring.releaseThrough(finished, [&](const VkCommandBuffer commands) { mSpare.push_back(commands); });
    }

    void CommandPool::dropDeferred()
    {
        recycle(mDeferred);
        mDeferred.clear();
    }

    void CommandPool::discard(VkCommandBuffer commands)
    {
        // Neither ended nor submitted: a buffer still being recorded is not pending, so this is
        // where a recording nobody wants goes back. Reset first, because a begin resets a buffer
        // that was ended and not one still recording.
        checkVk(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer");
        recycle(std::span<const VkCommandBuffer>(&commands, 1));
    }

    void CommandPool::recycle(std::span<const VkCommandBuffer> commands)
    {
        mSpare.insert(mSpare.end(), commands.begin(), commands.end());
    }

    VkCommandBuffer CommandPool::take()
    {
        if (!mSpare.empty())
        {
            const VkCommandBuffer spare = mSpare.back();
            mSpare.pop_back();
            return spare;
        }

        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = mHandle.get(),
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        VkCommandBuffer commands = VK_NULL_HANDLE;
        checkVk(vkAllocateCommandBuffers(mDevice.getHandle(), &allocate, &commands), "vkAllocateCommandBuffers");
        return commands;
    }

    std::vector<VkCommandBuffer> CommandPool::allocate(std::uint32_t count)
    {
        std::vector<VkCommandBuffer> buffers;
        buffers.reserve(count);
        for (std::uint32_t at = 0; at < count; ++at)
            buffers.push_back(take());

        return buffers;
    }

    std::size_t CommandPool::takeStaging(const VkDeviceSize bytes)
    {
        const Timeline& timeline = mDevice.getTimeline();
        for (std::size_t at = 0; at < mStaging.size(); ++at)
        {
            StagingBlock& block = mStaging[at];
            if (block.mTaken || !timeline.hasFinished(block.mReadUntil) || block.mBuffer.getSize() < bytes)
                continue;

            block.mTaken = true;
            return at;
        }

        mStaging.push_back(StagingBlock{
            .mBuffer = Buffer::staging(
                mDevice, std::max(bytes, sStagingBlock), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "staging block"),
            .mTaken = true,
        });
        return mStaging.size() - 1;
    }

    std::size_t CommandPool::takeHold()
    {
        if (!mFreeHolds.empty())
        {
            const std::size_t hold = mFreeHolds.back();
            mFreeHolds.pop_back();
            return hold;
        }

        mHolds.emplace_back();
        return mHolds.size() - 1;
    }

    void CommandPool::giveHold(const std::size_t hold)
    {
        [[maybe_unused]] const BatchHold& given = mHolds[hold];
        assert(given.mBuffers.empty() && given.mImages.empty() && given.mBlocks.empty()
            && "a hold given back still holding what its batch never released");
        mFreeHolds.push_back(hold);
    }

    void CommandPool::giveStaging(const std::size_t block, const std::uint64_t readUntil)
    {
        assert(mStaging[block].mTaken && "a staging block given back twice");

        mStaging[block].mReadUntil = readUntil;
        mStaging[block].mTaken = false;
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
        const VkCommandBuffer commands = take();
        begin(commands);
        return commands;
    }

    void CommandPool::endAndWait(VkCommandBuffer commands)
    {
        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

        mDevice.waitFor(submitWithDeferred(commands, {}, {}), "a one-off submit");

        // The copies have run, so every buffer that carried a deferred batch can go back; what
        // the batches read was buried when they were handed over, and the wait above collected
        // it.
        recycle(mDeferred);
        recycle(std::span<const VkCommandBuffer>(&commands, 1));
        mDeferred.clear();
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
        mPool.giveHold(mHold);
    }

    VkCommandBuffer Batch::getCommands()
    {
        if (mCommands == VK_NULL_HANDLE)
            mCommands = mPool.begin();

        return mCommands;
    }

    void Batch::keep(Buffer&& buffer)
    {
        mPool.holdAt(mHold).mBuffers.push_back(std::move(buffer));
    }

    void Batch::keep(Image&& image)
    {
        mPool.holdAt(mHold).mImages.push_back(std::move(image));
    }

    StagingRun Batch::stage(std::span<const std::byte> bytes)
    {
        VkDeviceSize at = alignUp(mFilled, sStagingAlignment);

        std::vector<std::size_t>& blocks = mPool.holdAt(mHold).mBlocks;
        if (blocks.empty() || at + bytes.size() > mPool.stagingAt(blocks.back()).getSize())
        {
            blocks.push_back(mPool.takeStaging(bytes.size()));
            at = 0;
        }

        const Buffer& block = mPool.stagingAt(blocks.back());
        block.writeAt(at, bytes);
        mFilled = at + bytes.size();

        return StagingRun{ .mBuffer = block.getHandle(), .mOffset = at };
    }

    void Batch::release()
    {
        CommandPool::BatchHold& hold = mPool.holdAt(mHold);
        Graveyard& graveyard = getDevice().getGraveyard();
        for (Buffer& buffer : hold.mBuffers)
            graveyard.bury(std::move(buffer));
        for (Image& image : hold.mImages)
            graveyard.bury(std::move(image));

        // Under the same value a burial would be, for the same reason.
        const std::uint64_t readUntil = getDevice().getTimeline().getNext();
        for (const std::size_t block : hold.mBlocks)
            mPool.giveStaging(block, readUntil);

        hold.mBuffers.clear();
        hold.mImages.clear();
        hold.mBlocks.clear();
        mFilled = 0;
    }

    void Batch::flush()
    {
        // Buried ahead of the submit, so the stamp is the value the wait below waits for, and the
        // wait's own collect is what frees them. Nothing recorded is a caller that kept something
        // and decided against the copy; what it kept may still have an earlier reader, and the
        // burial covers that too.
        release();

        if (mCommands != VK_NULL_HANDLE)
            mPool.endAndWait(std::exchange(mCommands, VK_NULL_HANDLE));
    }

    void Batch::defer()
    {
        // Buried under the next submit, which is the one the pool puts this batch ahead of.
        release();

        if (mCommands != VK_NULL_HANDLE)
            mPool.defer(std::exchange(mCommands, VK_NULL_HANDLE));
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
        // says so. The source needs no stamp: a staging block is the ring's, and the batch gives it
        // back stamped with the submit that carries it.
        into.nameForNext();
    }

    Buffer uploadBuffer(Batch& batch, std::span<const std::byte> bytes, VkBufferUsageFlags usage, std::string_view name)
    {
        Buffer result
            = Buffer::deviceLocal(batch.getDevice(), bytes.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, name);
        stageInto(batch, result, 0, bytes);

        // What makes an upload self-contained. Batched, the next thing recorded may be an
        // acceleration structure built out of exactly these bytes, and without this it would read
        // them before the copy had run.
        result.transition(batch.getCommands(), Use::sBufferCopyWrite, Use::sBufferAnyRead);

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
