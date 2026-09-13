#include "framering.hpp"

#include <cassert>
#include <chrono>

#include <components/rtx/frameclock.hpp>

#include "commands.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "timeline.hpp"

namespace Rtx
{
    FrameRecord::FrameRecord(const Device& device)
        : mTimer(device)
        , mHitCount(Buffer::staging(
              device, sizeof(FrameCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
    {
    }

    FrameRing::FrameRing(
        const Device& device, CommandPool& pool, Graveyard& graveyard, const bool countHits, const bool countCrossings)
        : mDevice(device)
        , mPool(pool)
        , mGraveyard(graveyard)
        , mCountHits(countHits)
        , mCountCrossings(countCrossings)
        , mSlots{ { FrameRecord{ device }, FrameRecord{ device } } }
    {
        // Three command buffers a frame to begin with — the first placement's, the trace's, the
        // interface's — allocated once and recorded into again. A frame placed more than once takes
        // another from the same pool and keeps it, which `FrameRecord::mPlaceCommands` explains.
        const std::vector<VkCommandBuffer> commands = mPool.allocate(3 * sFrameSlots);
        for (std::uint32_t slot = 0; slot < sFrameSlots; ++slot)
        {
            FrameRecord& frame = mSlots[slot];
            frame.mPlaceCommands.push_back(commands[3 * slot]);
            frame.mWorld.mCommands = commands[3 * slot + 1];
            frame.mGui.mCommands = commands[3 * slot + 2];
        }
    }

    FrameRecord& FrameRing::recording()
    {
        makeRoom();
        return slotOf(mFrame);
    }

    void FrameRing::makeRoom()
    {
        // The frame that last used the next slot has to be out of the way — waited for, its
        // burials collected, its results read or dropped — which is what caps the frames in flight
        // at the number of slots and what makes the slot `recording` hands back the caller's own.
        while (mFrame - mFinished >= sFrameSlots)
            finishOldest();
    }

    FrameRecord& FrameRing::begin()
    {
        FrameRecord& frame = recording();
        if (frame.mBegun)
            return frame;

        frame.mTimer.beginFrame(mFrame);
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
        frame.mWorld.mSubmitted = mPool.submit(frame.mWorld.mCommands, mGraveyard);

        frame.mBegun = false;
        frame.mWorld.mPending = true;
        ++mFrame;
        frame.mInFlight = static_cast<std::uint32_t>(mFrame - mFinished);
    }

    void FrameRing::finishOldest()
    {
        assert(mFinished < mFrame && "nothing in flight to finish");

        FrameRecord& frame = slotOf(mFinished);
        assert(frame.mWorld.mPending && "a frame in flight that was never submitted");

        const auto start = std::chrono::steady_clock::now();
        mDevice.getTimeline().waitFor(frame.mWorld.mSubmitted, "a frame");
        const double waited = since(start, std::chrono::steady_clock::now());

        frame.mWorld.mPending = false;

        // Read after the wait and never before: the count is the device's sum, and the queries
        // are the device's clock.
        FrameCounts counted;
        if (mCountHits || mCountCrossings)
            counted = *static_cast<const FrameCounts*>(frame.mHitCount.map());

        // What the timeline has passed is nothing's now, this frame's burials among it.
        mGraveyard.collect();

        ++mFinished;

        if (mReports.size() >= sFrameSlots)
            mReports.erase(mReports.begin());

        FrameResult& report = mReports.emplace_back(FrameResult{
            .mHits = counted.mHits,
            .mCrossings = counted.mCrossings,
            .mCrossingsMost = counted.mCrossingsMost,
            .mWaitMs = waited,
            .mInFlight = frame.mInFlight,
            .mReconstruction = frame.mReconstruction,
        });
        frame.mTimer.resolve(report.mGpu);
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

        return takeReport();
    }

    std::optional<FrameResult> FrameRing::collectFinished()
    {
        makeRoom();
        return takeReport();
    }

    std::optional<FrameResult> FrameRing::takeReport()
    {
        if (mReports.empty())
            return std::nullopt;

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

}
