#include "framering.hpp"

#include <cassert>
#include <chrono>

#include <components/rtx/frameclock.hpp>

#include "commands.hpp"
#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    FrameRecord::FrameRecord(const Device& device, CommandPool& pool)
        : mWorld(device, pool)
        , mGui(device, pool)
        , mTimer(device)
        , mHitCount(Buffer::staging(
              device, sizeof(FrameCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
    {
    }

    FrameRing::FrameRing(const Device& device, CommandPool& pool, const bool countHits, const bool countCrossings)
        : mDevice(device)
        , mPool(pool)
        , mCountHits(countHits)
        , mCountCrossings(countCrossings)
        , mSlots{ { FrameRecord{ device, pool }, FrameRecord{ device, pool } } }
    {
        // Three command buffers a frame to begin with — the first placement's, the trace's, the
        // interface's — allocated once and recorded into again, and a fence for each of the two that
        // are waited on. A frame placed more than once takes another from the same pool and keeps
        // it, which `FrameRecord::mPlaceCommands` explains.
        const std::vector<VkCommandBuffer> commands = mPool.allocate(3 * sFrameSlots);
        const VkFenceCreateInfo fence{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (std::uint32_t slot = 0; slot < sFrameSlots; ++slot)
        {
            FrameRecord& frame = mSlots[slot];
            frame.mPlaceCommands.push_back(commands[3 * slot]);
            frame.mWorld.mCommands = commands[3 * slot + 1];
            frame.mGui.mCommands = commands[3 * slot + 2];
            checkVk(vkCreateFence(mDevice.getHandle(), &fence, nullptr, frame.mWorld.mFence.put(mDevice.getHandle())),
                "vkCreateFence");
            checkVk(vkCreateFence(mDevice.getHandle(), &fence, nullptr, frame.mGui.mFence.put(mDevice.getHandle())),
                "vkCreateFence");
        }
    }

    FrameRecord& FrameRing::recording()
    {
        // The frame that last used this slot has to be out of the way — its fence waited, its
        // graveyard emptied, its results read or dropped — which is what caps the frames in flight
        // at the number of slots and what makes the slot this hands back the caller's own.
        while (mFrame - mFinished >= sFrameSlots)
            finishOldest();

        return slotOf(mFrame);
    }

    FrameRecord& FrameRing::begin()
    {
        FrameRecord& frame = recording();
        if (frame.mBegun)
            return frame;

        frame.mTimer.beginFrame();
        frame.mBegun = true;
        frame.mPlacements = 0;
        frame.mReconstruction = Reconstruction{};
        return frame;
    }

    VkCommandBuffer FrameRing::takePlaceCommands(FrameRecord& frame)
    {
        if (frame.mPlacements == frame.mPlaceCommands.size())
            frame.mPlaceCommands.push_back(mPool.allocate(1).front());

        return frame.mPlaceCommands[frame.mPlacements++];
    }

    void FrameRing::submit(FrameRecord& frame)
    {
        mPool.submit(frame.mWorld.mCommands, frame.mWorld.mFence.get(), frame.mWorld.mGraveyard);

        frame.mBegun = false;
        frame.mWorld.mPending = true;
        ++mFrame;
    }

    void FrameRing::finishOldest()
    {
        assert(mFinished < mFrame && "nothing in flight to finish");

        FrameRecord& frame = slotOf(mFinished);
        assert(frame.mWorld.mPending && "a frame in flight that was never submitted");

        const auto start = std::chrono::steady_clock::now();
        awaitVk(mDevice, frame.mWorld.mFence.get(), "a frame");
        const double waited = since(start, std::chrono::steady_clock::now());

        frame.mWorld.mPending = false;

        // Read after the fence and never before: the count is the device's sum, and the queries
        // are the device's clock.
        FrameCounts counted;
        if (mCountHits || mCountCrossings)
            counted = *static_cast<const FrameCounts*>(frame.mHitCount.map());

        // What this frame may still have been reading is nothing's now.
        frame.mWorld.mGraveyard.clear();

        ++mFinished;

        // `FrameResult::mGpu` is a span into the frame's own timer, good until that slot resolves
        // again `sFrameSlots` finishes away, so a report nothing collected by then goes rather
        // than hand back a later frame's zones.
        if (mReports.size() >= sFrameSlots)
            mReports.erase(mReports.begin());

        mReports.push_back(FrameResult{
            .mHits = counted.mHits,
            .mCrossings = counted.mCrossings,
            .mCrossingsMost = counted.mCrossingsMost,
            .mWaitMs = waited,
            .mGpu = frame.mTimer.resolve(),
            .mReconstruction = frame.mReconstruction,
        });
    }

    std::optional<FrameResult> FrameRing::collect()
    {
        // What is already in hand before anything is waited for. A frame the ring drained to
        // make room has been finished and its report is here; waiting again would wait the frame
        // after it and hand back a report a frame ahead of the one the caller is asking about.
        if (mReports.empty())
        {
            if (mFinished == mFrame)
                return std::nullopt;

            finishOldest();
        }

        const FrameResult report = mReports.front();
        mReports.erase(mReports.begin());
        return report;
    }

    void FrameRing::finishThrough(const std::uint64_t frame)
    {
        while (mFinished < mFrame && mFinished <= frame)
            finishOldest();
    }

    void FrameRing::finishAll()
    {
        while (mFinished < mFrame)
            finishOldest();
    }

    void FrameRing::emptyGraveyards()
    {
        for (FrameRecord& frame : mSlots)
        {
            frame.mWorld.mGraveyard.clear();
            frame.mGui.mGraveyard.clear();
        }
    }

}
