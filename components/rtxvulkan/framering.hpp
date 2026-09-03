#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <vulkan/vulkan.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

#include "buffer.hpp"
#include "frameslots.hpp"
#include "gputimer.hpp"
#include "graveyard.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// Everything one frame in flight owns: what it records into, what says it is done, what it
    /// measured, and what it may still be reading.
    ///
    /// **Two of these, and the CPU works one ahead of the GPU.** Frame N+1 is walked and placed
    /// while frame N is traced; what N+1 writes is this frame's copy of every table, and what N
    /// may still read is the other's. The frame after next takes this one's place, and waits
    /// its fence first.
    struct FrameSlot
    {
        FrameSlot(const Device& device, CommandPool& pool);

        /// The placements' commands and the trace's, submitted apart because a picture inside
        /// the interface is traced between the two and needs the first to have reached the
        /// queue. Only the trace carries the fence: it is later on the queue, so its signal
        /// covers every placement before it.
        ///
        /// **One buffer per placement, because a frame may be placed more than once.** A cell
        /// crossing hands the scene over twice — once for what arrived and once for the walk
        /// that follows — and the game walks its precipitation beside its world. Two placements
        /// sharing a buffer is a recording over a submit already in flight, so each takes its
        /// own and the frame stays one frame: what the ring counts is what the caller drew.
        ///
        /// Grown to the busiest frame so far and never freed. The pool is never reset, so what
        /// it handed out stays good for the life of the renderer.
        std::vector<VkCommandBuffer> mPlaceCommands;
        std::size_t mPlacements = 0;

        VkCommandBuffer mCommands = VK_NULL_HANDLE;
        VkFence mFence = VK_NULL_HANDLE;

        /// Begun by a placement or a trace and not yet submitted with its fence.
        bool mBegun = false;

        /// Submitted with its fence and not yet waited for.
        bool mPending = false;

        /// Its own timer and its own count, because both are read after the fence, when the
        /// next frame is already writing its own.
        GpuTimer mTimer;
        Buffer mHitCount;

        /// What this frame may still be reading, destroyed when its fence says it is not.
        Graveyard mGraveyard;
        Reconstruction mReconstruction;

        /// The interface's own ring beside the frame's: it is drawn after the frame is submitted
        /// and fenced on its own, so its vertices are guarded by its own fence.
        VkCommandBuffer mGuiCommands = VK_NULL_HANDLE;
        VkFence mGuiFence = VK_NULL_HANDLE;
        bool mGuiPending = false;

        /// What the GUI is drawn out of, rewritten every frame it has anything in it and grown
        /// to the busiest frame so far. Host-visible device memory, so writing it is a memcpy
        /// and there is no staging copy and no transfer to record.
        Buffer mGuiVertices;
        Graveyard mGuiGraveyard;
    };

    /// The frames in flight, and the discipline that keeps them apart.
    ///
    /// **Two slots, and the CPU works one ahead of the GPU.** Frame N+1 is walked and placed while
    /// frame N is traced; the frame after next takes N's slot and waits its fence first, which is
    /// what caps the frames in flight at the number of slots.
    ///
    /// **A report belongs to its frame and not to whichever call did the waiting.** Making room in
    /// the ring finishes a frame, and its report used to go on the floor — so a caller asking once
    /// a frame was answered for fewer than half of them. They queue here instead.
    class FrameRing
    {
    public:
        /// @param countHits whether a frame's hit count is worth reading back. Borrowed from the
        ///        renderer, which decides it once and compiles its pipeline against the same answer.
        FrameRing(const Device& device, CommandPool& pool, const bool& countHits);
        ~FrameRing();

        FrameRing(const FrameRing&) = delete;
        FrameRing& operator=(const FrameRing&) = delete;

        /// The slot of the frame being recorded, whether or not anything has begun it yet.
        FrameSlot& recording() { return slotOf(mFrame); }

        /// The slot `frame` used, for a caller counting on a ring of its own — the interface's.
        FrameSlot& slotOf(std::uint64_t frame) { return mSlots[frame % sFrameSlots]; }

        /// How many frames have been submitted, which is the number the next one will carry.
        std::uint64_t getRecording() const { return mFrame; }

        /// The frame being recorded, begun if it was not: the frame that last used its slot is
        /// waited for, its fence reset, its timer and hit count cleared.
        FrameSlot& begin();

        /// A command buffer for one placement of `frame`, made on the frame that first needs it.
        VkCommandBuffer takePlaceCommands(FrameSlot& frame);

        /// Submits what a frame recorded, under its own fence, and counts it as in flight.
        void submit(FrameSlot& frame);

        /// The oldest report in hand, waiting a frame out for one where there is none.
        std::optional<FrameResult> collect();

        /// Waits until `frame` is finished, where it was ever submitted.
        void finishThrough(std::uint64_t frame);

        /// Waits for every frame in flight. What an arrival, a rebuild, a resize and a picture
        /// inside the interface do first.
        void finishAll();

        /// Destroys what every frame is holding, whether or not its slot ever comes round again.
        ///
        /// **After `waitIdle`, and only where something buried is about to lose its owner.** A room
        /// is a pointer into a scene's structure storage and a structure stands in that storage, so
        /// a frame that placed a scene and was never traced would give both back to a scene that no
        /// longer exists. `finishAll` cannot reach that frame: it was never submitted.
        void emptyGraveyards();

        /// Drops what nothing has collected, for a caller whose world has gone.
        void dropReports() { mReports.clear(); }

    private:
        /// Waits the oldest frame in flight out and puts what it came to in `mReports`.
        void finishOldest();

        const Device& mDevice;
        CommandPool& mPool;

        const bool& mCountHits;

        std::array<FrameSlot, sFrameSlots> mSlots;

        /// The next frame to record and the next to finish. Everything from `mFinished` to `mFrame`
        /// is in flight, and there are never more of those than there are slots.
        std::uint64_t mFrame = 0;
        std::uint64_t mFinished = 0;

        /// What frames have come to and nothing has asked for yet, oldest first.
        ///
        /// **A frame's report belongs to the frame and not to whichever call did the waiting.**
        /// `beginFrame` waits a slot out when the ring is full, and the report of the frame it
        /// waited used to go on the floor — so a caller asking once a frame was answered for fewer
        /// than half of them, and a run's figures were a sample of whichever frames it reached.
        ///
        /// **Never longer than `sFrameSlots`, because that is how long a report stays true.**
        /// `FrameResult::mGpu` is a span into the frame's own timer and the slot resolves again
        /// when it comes round, so a report held past that would carry another frame's zones.
        /// `finishOldest` drops the oldest rather than let that happen, and a caller asking once a
        /// frame never gets near it. A new world drops what is left, and `setScene` says why.
        std::vector<FrameResult> mReports;
    };
}
