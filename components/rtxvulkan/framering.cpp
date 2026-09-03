#include "framering.hpp"

#include <cassert>
#include <chrono>

#include "commands.hpp"
#include "device.hpp"
#include "result.hpp"

#include <components/rtx/frametimes.hpp>

namespace Rtx
{
    FrameSlot::FrameSlot(const Device& device, CommandPool& pool)
        : mTimer(device)
        , mHitCount(Buffer::staging(
              device, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        , mGraveyard(device, pool)
        , mGuiGraveyard(device, pool)
    {
    }

    FrameRing::FrameRing(const Device& device, CommandPool& pool, const bool& countHits)
        : mDevice(device)
        , mPool(pool)
        , mCountHits(countHits)
        , mSlots{ { FrameSlot{ device, pool }, FrameSlot{ device, pool } } }
    {
        // Three command buffers a frame to begin with — the first placement's, the trace's, the
        // interface's — allocated once and recorded into again, and a fence for each of the two that
        // are waited on. A frame placed more than once takes another from the same pool and keeps
        // it, which `FrameSlot::mPlaceCommands` explains.
        const std::vector<VkCommandBuffer> commands = mPool.allocate(3 * sFrameSlots);
        const VkFenceCreateInfo fence{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (std::uint32_t slot = 0; slot < sFrameSlots; ++slot)
        {
            FrameSlot& frame = mSlots[slot];
            frame.mPlaceCommands.push_back(commands[3 * slot]);
            frame.mCommands = commands[3 * slot + 1];
            frame.mGuiCommands = commands[3 * slot + 2];
            checkVk(vkCreateFence(mDevice.getHandle(), &fence, nullptr, &frame.mFence), "vkCreateFence");
            checkVk(vkCreateFence(mDevice.getHandle(), &fence, nullptr, &frame.mGuiFence), "vkCreateFence");
        }
    }

    FrameRing::~FrameRing()
    {
        // **Whatever the frames are still holding goes first.** A room is a pointer into a scene's
        // structure storage, and the renderer empties the graveyards before it takes its scenes
        // apart; this is the fences the slots themselves own.
        for (FrameSlot& frame : mSlots)
        {
            vkDestroyFence(mDevice.getHandle(), frame.mFence, nullptr);
            vkDestroyFence(mDevice.getHandle(), frame.mGuiFence, nullptr);
        }
    }

    FrameSlot& FrameRing::begin()
    {
        FrameSlot& frame = slotOf(mFrame);
        if (frame.mBegun)
            return frame;

        // **The frame that last used this slot has to be out of the way** — its fence waited, its
        // graveyard emptied, its results read or dropped — which is what caps the frames in flight
        // at the number of slots.
        while (mFrame - mFinished >= sFrameSlots)
            finishOldest();

        frame.mTimer.beginFrame();
        frame.mBegun = true;
        frame.mPlacements = 0;
        frame.mReconstruction = Reconstruction{};
        return frame;
    }

    VkCommandBuffer FrameRing::takePlaceCommands(FrameSlot& frame)
    {
        if (frame.mPlacements == frame.mPlaceCommands.size())
            frame.mPlaceCommands.push_back(mPool.allocate(1).front());

        return frame.mPlaceCommands[frame.mPlacements++];
    }

    void FrameRing::submit(FrameSlot& frame)
    {
        mPool.submit(frame.mCommands, frame.mFence, frame.mGraveyard);

        frame.mBegun = false;
        frame.mPending = true;
        ++mFrame;
    }

    void FrameRing::finishOldest()
    {
        assert(mFinished < mFrame && "nothing in flight to finish");

        FrameSlot& frame = slotOf(mFinished);
        assert(frame.mPending && "a frame in flight that was never submitted");

        const auto start = std::chrono::steady_clock::now();
        awaitVk(mDevice, frame.mFence, "a frame");
        const double waited = since(start, std::chrono::steady_clock::now());

        frame.mPending = false;

        // Read after the fence and never before: the count is the device's sum, and the queries
        // are the device's clock.
        std::uint32_t hits = 0;
        if (mCountHits)
            hits = *static_cast<const std::uint32_t*>(frame.mHitCount.map());

        // What this frame may still have been reading is nothing's now.
        frame.mGraveyard.clear();

        ++mFinished;

        // **What the ring can hold is what a report stays valid for.** `FrameResult::mGpu` is a
        // span into the frame's own timer, good until that slot comes round and resolves again —
        // which is `sFrameSlots` finishes away. A report nothing has collected by then would hand
        // back zones belonging to a later frame, so the oldest goes. A caller that asks once a
        // frame never reaches this: it leaves at most one here.
        if (mReports.size() >= sFrameSlots)
            mReports.erase(mReports.begin());

        mReports.push_back(FrameResult{
            .mHits = hits,
            .mWaitMs = waited,
            .mGpu = frame.mTimer.resolve(),
            .mReconstruction = frame.mReconstruction,
        });
    }

    std::optional<FrameResult> FrameRing::collect()
    {
        // **What is already in hand before anything is waited for.** A frame the ring drained to
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
        for (FrameSlot& frame : mSlots)
        {
            frame.mGraveyard.clear();
            frame.mGuiGraveyard.clear();
        }
    }

}
