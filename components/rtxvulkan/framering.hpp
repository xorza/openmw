#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

#include "buffer.hpp"
#include "frameslots.hpp"
#include "gputimer.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;
    class Graveyard;

    /// What the trace counts over a frame: the host's spelling of `lib/bindings.glsl`'s `HitCount`
    /// block, so the frame that clears the buffer and the frame that reads it back agree about
    /// where each word sits.
    struct FrameCounts
    {
        std::uint32_t mHits = 0;
        std::uint32_t mCrossings = 0;
        std::uint32_t mCrossingsMost = 0;
    };

    /// One command buffer and the timeline value it was submitted under. Held twice per frame,
    /// because a frame submits twice: the world, and the interface over it.
    struct Submission
    {
        VkCommandBuffer mCommands = VK_NULL_HANDLE;

        /// What the submit signalled on the device's timeline, which is what says it has run.
        std::uint64_t mSubmitted = 0;

        /// Submitted and not yet waited for.
        bool mPending = false;
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
        /// and waited for on its own, so its vertices are guarded by its own value.
        Submission mGui;

        /// Begun by a placement or a trace and not yet submitted.
        bool mBegun = false;

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
    };

    /// The frames in flight, and the discipline that keeps them apart: two slots, and the CPU
    /// works one ahead of the GPU. Frame N+1 is walked and placed while frame N is traced; the
    /// frame after next takes N's slot and waits for it first. A report belongs to its frame
    /// and queues here when making room finished it, or a caller asking once a frame would be
    /// answered for fewer than half of them.
    class FrameRing
    {
    public:
        /// @param graveyard the renderer's, collected after every wait this ring makes, because a
        ///        wait is where what the timeline is known to have passed changes.
        /// @param countHits,countCrossings whether either of a frame's counts is worth reading
        ///        back. Borrowed from the renderer, which decides both once and compiles its
        ///        pipeline against the same answers.
        FrameRing(const Device& device, CommandPool& pool, Graveyard& graveyard, bool countHits, bool countCrossings);

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

        /// Every frame below this has been waited for.
        std::uint64_t getFinished() const { return mFinished; }

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

        /// Waits until `frame` is finished, where it was ever submitted.
        void finishThrough(std::uint64_t frame);

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
        CommandPool& mPool;
        Graveyard& mGraveyard;

        /// By value, because both are settled at construction and never move. References into
        /// the renderer's own members would tie this ring's correctness to where two booleans
        /// happen to live.
        bool mCountHits = false;
        bool mCountCrossings = false;

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
