#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/stepped.hpp>

#include "buffer.hpp"
#include "frameslots.hpp"
#include "gputimer.hpp"

namespace Rtx
{
    class Device;

    /// What the trace counts over a frame: the host's spelling of `lib/bindings.glsl`'s `HitCount`
    /// block, so the frame that clears the buffer and the frame that reads it back agree about
    /// where each word sits.
    struct FrameCounts
    {
        std::uint32_t mHits = 0;
    };

    /// One command buffer and the timeline value it was submitted under.
    struct Submission
    {
        VkCommandBuffer mCommands = VK_NULL_HANDLE;

        /// What the submit signalled on the device's timeline, which is what says it has run.
        std::uint64_t mSubmitted = 0;
    };

    /// Where a frame's slot stands between one use and the next: nothing recorded, begun by a
    /// placement or a trace and not yet submitted, or submitted and not yet waited for. One
    /// step and not two flags, so a frame submitted twice or never begun is a call out of its
    /// turn and not a wait on a value nothing signals.
    enum class FrameState
    {
        Idle,
        Begun,
        Submitted,
    };

    struct FrameRecord
    {
        explicit FrameRecord(const Device& device);

        /// The placements' commands and the trace's, submitted apart because a picture inside
        /// the interface is traced between the two. Only the trace's value is waited on: it is
        /// later on the queue, so its signal covers every placement before it. One buffer per
        /// placement, because a cell crossing places twice and two placements sharing a buffer
        /// is a recording over a submit in flight. Grown to the busiest frame so far and never
        /// freed.
        std::vector<VkCommandBuffer> mPlaceCommands;
        std::size_t mPlacements = 0;

        /// The world's: every placement of this frame, then the trace.
        Submission mWorld;

        /// The interface's own ring beside the frame's: it is drawn after the frame is submitted
        /// and waited for on its own, by the stamp its vertices carry.
        VkCommandBuffer mGuiCommands = VK_NULL_HANDLE;

        Stepped<FrameState> mState{ FrameState::Idle };

        /// `FrameResult::mInFlight`, taken at the submit.
        std::uint32_t mInFlight = 0;

        /// Its own timer and its own counters, because both are read after the wait, when the
        /// next frame is already writing its own.
        GpuTimer mTimer;
        Buffer mHitCount;

        Reconstruction mReconstruction;

        /// What the GUI is drawn out of, rewritten every frame it has anything in it and grown
        /// to the busiest frame so far. Host-visible device memory, so writing it is a memcpy
        /// and there is no staging copy and no transfer to record.
        Buffer mGuiVertices;

        /// The debug lines' vertices, the same way, in the frame's own commands: the slot is the
        /// frame's, so a write lands under no submit in flight.
        Buffer mDebugVertices;

        /// Where the frame's picture lands where `FrameOptions::mReadBack` asks, grown to the
        /// picture on the first frame that asks and kept. `mReadBackBytes` is how much of it this
        /// frame wrote, nought for a frame that did not ask.
        Buffer mReadBack;
        VkDeviceSize mReadBackBytes = 0;
    };

    /// The frames in flight, and the discipline that keeps them apart: two slots, and the CPU
    /// works one ahead of the GPU. Frame N+1 is walked and placed while frame N is traced; the
    /// frame after next takes N's slot and waits for it first. A report belongs to its frame
    /// and queues here when making room finished it, or a caller asking once a frame would be
    /// answered for fewer than half of them.
    class FrameRing
    {
    public:
        /// @param countHits whether a frame's count is worth reading back. Borrowed from the
        ///        renderer, which decides it once and compiles its pipeline against the same answer.
        FrameRing(const Device& device, bool countHits);

        FrameRing(const FrameRing&) = delete;
        FrameRing& operator=(const FrameRing&) = delete;

        /// The slot of the frame being recorded, with whatever last used it finished. It does not
        /// open the frame, which `begin` is for.
        FrameRecord& recording();

        /// The slot `frame` used, for a caller counting on a ring of its own — the interface's.
        FrameRecord& slotOf(std::uint64_t frame)
        {
            return mSlots.at(FrameSlot{ static_cast<std::uint32_t>(frame % sFrameSlots) });
        }

        /// How many frames have been submitted, which is the number the next one will carry.
        std::uint64_t getRecording() const { return mFrame; }

        /// The slot the frame being recorded uses, for whatever else keeps one of a thing per
        /// frame in flight.
        FrameSlot getRecordingSlot() const { return FrameSlot{ static_cast<std::uint32_t>(mFrame % sFrameSlots) }; }

        /// The frame being recorded, begun if it was not: the frame that last used its slot is
        /// waited for, its timer and hit count cleared.
        FrameRecord& begin();

        /// A command buffer for one placement of `frame`, made on the frame that first needs it.
        VkCommandBuffer takePlaceCommands(FrameRecord& frame);

        /// Submits what a frame recorded and counts it as in flight.
        void submit(FrameRecord& frame);

        /// The oldest report in hand, waiting a frame out for one where there is none.
        std::optional<FrameResult> collect();

        /// The oldest report in hand, waiting only where the ring has no room for the next frame
        /// — `Renderer::collectFrame`.
        std::optional<FrameResult> collectFinished();

        /// Waits for every frame in flight. What an arrival, a rebuild, a resize and a picture
        /// inside the interface do first.
        void finishAll();

        /// Drops what nothing has collected, for a caller whose world has gone.
        void dropReports() { mReports.clear(); }

    private:
        /// Waits the oldest frame in flight out and puts what it came to in `mReports`.
        void finishOldest();

        /// Waits until the ring has a slot for the next frame.
        void makeRoom();

        std::optional<FrameResult> takeReport();

        const Device& mDevice;

        /// By value, because it is settled at construction and never moves. A reference into the
        /// renderer's own members would tie this ring's correctness to where a boolean happens to
        /// live.
        bool mCountHits = false;

        PerSlot<FrameRecord> mSlots;

        /// The next frame to record and the next to finish. Everything from `mFinished` to `mFrame`
        /// is in flight, and there are never more of those than there are slots.
        std::uint64_t mFrame = 0;
        std::uint64_t mFinished = 0;

        /// What frames have come to and nothing has asked for yet, oldest first. Never longer than
        /// `sFrameSlots`: a caller that stopped asking is not a reason to grow, and the oldest is
        /// the one furthest from what it asks about next, so `finishOldest` drops it.
        std::vector<FrameResult> mReports;
    };
}
