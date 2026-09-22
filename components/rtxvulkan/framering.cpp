#include "framering.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include <components/rtx/framedigest.hpp>
#include <components/rtx/framespend.hpp>
#include <components/rtx/shaders/digest.h>

#include "commands.hpp"
#include "device.hpp"
#include "digestpass.hpp"
#include "timeline.hpp"

namespace Rtx
{
    namespace
    {
        /// The words the pass folded, each image's four lanes read as two words.
        void readDigest(const Buffer& lanes, FrameDigest& into)
        {
            const auto* const words = static_cast<const std::uint32_t*>(lanes.map());
            for (std::size_t image = 0; image < into.mImages.size(); ++image)
            {
                const std::uint32_t* const lane = words + image * Shaders::DIGEST_LANES;
                into.mImages[image] = DigestWords{ lane[0] | (std::uint64_t{ lane[1] } << 32),
                    lane[2] | (std::uint64_t{ lane[3] } << 32) };
            }
        }
    }

    FrameRecord::FrameRecord(const Device& device)
        : mTimer(device)
        , mCounts(Buffer::readBack(device, sizeof(Shaders::FrameCounts),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "frame counts"))
        , mDigestLanes(Buffer::readBack(device, DigestPass::sBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "frame digest"))
    {
    }

    FrameRing::FrameRing(const Device& device, const bool readsCounts)
        : mDevice(device)
        , mReadsCounts(readsCounts)
        , mSlots([&](FrameSlot) { return FrameRecord{ device }; })
    {
        // Three command buffers a frame to begin with — the first placement's, the trace's, the
        // interface's — allocated once and recorded into again. A frame placed more than once takes
        // another from the same pool and keeps it, which `FrameRecord::mPlaceCommands` explains.
        const std::vector<VkCommandBuffer> commands = mDevice.getPool().allocate(3 * sFrameSlots);
        for (std::uint32_t slot = 0; slot < sFrameSlots; ++slot)
        {
            FrameRecord& frame = mSlots.at(FrameSlot{ slot });
            frame.mPlaceCommands.push_back(commands[3 * slot]);
            frame.mWorld.mCommands = commands[3 * slot + 1];
            frame.mGuiCommands = commands[3 * slot + 2];
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
        if (frame.mState.get() == FrameState::Begun)
            return frame;
        frame.mState.step(FrameState::Begun, FrameState::Idle);

        frame.mTimer.beginFrame(mFrame);
        frame.mPlacements = 0;
        frame.mReconstruction = Reconstruction{};
        frame.mReadBackBytes = 0;
        frame.mDigest = std::nullopt;
        return frame;
    }

    VkCommandBuffer FrameRing::takePlaceCommands(FrameRecord& frame)
    {
        if (frame.mPlacements == frame.mPlaceCommands.size())
            frame.mPlaceCommands.push_back(mDevice.getPool().take());

        return frame.mPlaceCommands[frame.mPlacements++];
    }

    void FrameRing::submit(FrameRecord& frame)
    {
        close(frame, true);
    }

    void FrameRing::skip()
    {
        FrameRecord& frame = slotOf(mFrame);
        mDevice.getPool().begin(frame.mWorld.mCommands);
        close(frame, false);
    }

    void FrameRing::close(FrameRecord& frame, const bool traced)
    {
        frame.mState.step(FrameState::Submitted, FrameState::Begun);
        frame.mTraced = traced;
        frame.mWorld.mSubmitted = mDevice.getPool().submit(frame.mWorld.mCommands);
        ++mFrame;
        frame.mInFlight = static_cast<std::uint32_t>(mFrame - mFinished);
    }

    void FrameRing::finishOldest()
    {
        assert(mFinished < mFrame && "nothing in flight to finish");

        FrameRecord& frame = slotOf(mFinished);
        frame.mState.expect(FrameState::Submitted);

        const auto start = std::chrono::steady_clock::now();
        mDevice.waitFor(frame.mWorld.mSubmitted, "a frame");
        const double waited = since(start, std::chrono::steady_clock::now());

        frame.mState.step(FrameState::Idle, FrameState::Submitted);

        if (!frame.mTraced)
        {
            ++mFinished;
            return;
        }

        // Read after the wait and never before: the counts are the device's, and the queries
        // are the device's clock.
        Shaders::FrameCounts counted{};
        if (mReadsCounts)
            counted = *static_cast<const Shaders::FrameCounts*>(frame.mCounts.map());

        if (mReports.size() >= sFrameSlots)
            mReports.erase(mReports.begin());

        // Handed out as a span over the ring's own memory, which `pictureOf` says how long stands.
        const std::span<const std::uint8_t> pixels = frame.mReadBackBytes > 0
            ? std::span(static_cast<const std::uint8_t*>(pictureOf(mFinished).map()), frame.mReadBackBytes)
            : std::span<const std::uint8_t>();

        if (frame.mDigest.has_value())
            readDigest(frame.mDigestLanes, *frame.mDigest);

        assert(counted.mMisses <= frame.mCountedRays && "more primary rays missed than were launched");

        FrameResult& report = mReports.emplace_back(FrameResult{
            .mHits = frame.mCountedRays - counted.mMisses,
            .mNotFinite = NotFinite{ .mFog = counted.mNotFinite[Shaders::BOUNDARY_FOG],
                .mColour = counted.mNotFinite[Shaders::BOUNDARY_COLOUR],
                .mGuide = counted.mNotFinite[Shaders::BOUNDARY_GUIDE] },
            .mHeldMs = counted.mHeldNs * 1.0e-6,
            .mWaitMs = waited,
            .mInFlight = frame.mInFlight,
            .mReconstruction = frame.mReconstruction,
            .mFrame = mFinished,
            .mPixels = pixels,
            .mDigest = frame.mDigest,
        });
        ++mFinished;
        frame.mTimer.resolve(report.mGpu);
    }

    std::optional<FrameResult> FrameRing::collect()
    {
        // What is already in hand before anything is waited for. A frame the ring drained to
        // make room has been finished and its report is here; waiting again would wait the frame
        // after it and hand back a report a frame ahead of the one the caller is asking about.
        //
        // Waited out one after another where the oldest were skipped, which come back with no
        // report.
        while (mReports.empty() && mFinished < mFrame)
            finishOldest();

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

    void FrameRing::finishAll()
    {
        while (mFinished < mFrame)
            finishOldest();
    }

}
