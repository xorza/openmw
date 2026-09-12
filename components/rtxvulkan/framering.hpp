#pragma once

#include <array>
#include <cstddef>
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
#include "owned.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// What the trace counts over a frame, laid out as the shader writes it.
    ///
    /// **The host's spelling of `lib/bindings.glsl`'s `HitCount` block**, so the frame that clears
    /// the buffer and the frame that reads it back agree about where each word sits.
    struct FrameCounts
    {
        std::uint32_t mHits = 0;
        std::uint32_t mCrossings = 0;
        std::uint32_t mCrossingsMost = 0;
    };

    /// Everything one frame in flight owns: what it records into, what says it is done, what it
    /// measured, and what it may still be reading.
    ///
    /// **Two of these, and the CPU works one ahead of the GPU.** Frame N+1 is walked and placed
    /// while frame N is traced; what N+1 writes is this frame's copy of every table, and what N
    /// may still read is the other's. The frame after next takes this one's place, and waits
    /// its fence first.
    /// One command buffer, the fence it is submitted with, and what that fence guards.
    ///
    /// **Named once and held twice**, because a frame submits twice: the world, and the interface
    /// over it. The two were eight fields whose only difference was a `mGui` prefix, so a field
    /// added to one was a field the other went without.
    struct Submission
    {
        Submission(const Device& device, CommandPool& pool)
            : mGraveyard(device, pool)
        {
        }

        VkCommandBuffer mCommands = VK_NULL_HANDLE;
        Owned<VkFence, vkDestroyFence> mFence;

        /// Submitted with its fence and not yet waited for.
        bool mPending = false;

        /// What this submission may still be reading, destroyed when its fence says it is not.
        Graveyard mGraveyard;
    };

    struct FrameRecord
    {
        FrameRecord(const Device& device, CommandPool& pool);

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

        /// The world's: every placement of this frame, then the trace.
        Submission mWorld;

        /// The interface's own ring beside the frame's: it is drawn after the frame is submitted
        /// and fenced on its own, so its vertices are guarded by its own fence.
        Submission mGui;

        /// Begun by a placement or a trace and not yet submitted with its fence.
        bool mBegun = false;

        /// Its own timer and its own counters, because both are read after the fence, when the
        /// next frame is already writing its own.
        GpuTimer mTimer;
        Buffer mHitCount;

        Reconstruction mReconstruction;

        /// What the GUI is drawn out of, rewritten every frame it has anything in it and grown
        /// to the busiest frame so far. Host-visible device memory, so writing it is a memcpy
        /// and there is no staging copy and no transfer to record.
        Buffer mGuiVertices;
    };

    /// The frames in flight, and the discipline that keeps them apart.
    ///
    /// **Two slots, and the CPU works one ahead of the GPU.** Frame N+1 is walked and placed while
    /// frame N is traced; the frame after next takes N's slot and waits its fence first, which is
    /// what caps the frames in flight at the number of slots.
    ///
    /// **A report belongs to its frame and not to whichever call did the waiting.** Making room in
    /// the ring finishes a frame, and its report queues here rather than going on the floor — or a
    /// caller asking once a frame is answered for fewer than half of them.
    class FrameRing
    {
    public:
        /// @param countHits,countCrossings whether either of a frame's counts is worth reading
        ///        back. Borrowed from the renderer, which decides both once and compiles its
        ///        pipeline against the same answers.
        FrameRing(const Device& device, CommandPool& pool, bool countHits, bool countCrossings);

        FrameRing(const FrameRing&) = delete;
        FrameRing& operator=(const FrameRing&) = delete;

        /// The slot of the frame being recorded, with whatever last used it finished.
        ///
        /// **It drains, and that is the whole of what makes its graveyard safe to bury in.** There
        /// are two slots and two frames may be in flight, so `slotOf(mFrame)` is `slotOf(mFinished)`
        /// — the slot of the *oldest frame still on the queue*. Anything buried in that slot's
        /// graveyard is destroyed by the next `finishOldest`, and that call waits for the oldest
        /// frame alone: the newer one is still tracing. `VulkanRenderer::dropTextures` buries a
        /// texture the scene let go, before any of the calls that drain, and the image went under a
        /// trace whose descriptor set still named it — a device lost with an invalid read and no
        /// other sign. Every other caller happened to have drained already, and none of them said so.
        ///
        /// **It does not open the frame, which `begin` is for.** A picture inside the interface
        /// takes a graveyard and must not start the frame's timer.
        FrameRecord& recording();

        /// The slot `frame` used, for a caller counting on a ring of its own — the interface's.
        FrameRecord& slotOf(std::uint64_t frame) { return mSlots[frame % sFrameSlots]; }

        /// How many frames have been submitted, which is the number the next one will carry.
        std::uint64_t getRecording() const { return mFrame; }

        /// Every frame below this has been waited for.
        std::uint64_t getFinished() const { return mFinished; }

        /// The frame being recorded, begun if it was not: the frame that last used its slot is
        /// waited for, its fence reset, its timer and hit count cleared.
        FrameRecord& begin();

        /// A command buffer for one placement of `frame`, made on the frame that first needs it.
        VkCommandBuffer takePlaceCommands(FrameRecord& frame);

        /// Submits what a frame recorded, under its own fence, and counts it as in flight.
        void submit(FrameRecord& frame);

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

        /// **By value, because both are settled at construction and never move.** References into
        /// the renderer's own members would tie this ring's correctness to where two booleans
        /// happen to live.
        bool mCountHits = false;
        bool mCountCrossings = false;

        std::array<FrameRecord, sFrameSlots> mSlots;

        /// The next frame to record and the next to finish. Everything from `mFinished` to `mFrame`
        /// is in flight, and there are never more of those than there are slots.
        std::uint64_t mFrame = 0;
        std::uint64_t mFinished = 0;

        /// What frames have come to and nothing has asked for yet, oldest first.
        ///
        /// **A frame's report belongs to the frame and not to whichever call did the waiting.**
        /// `beginFrame` waits a slot out when the ring is full, and the report of the frame it
        /// waited goes here — or a caller asking once a frame is answered for fewer than half of
        /// them, and a run's figures are a sample of whichever frames it reached.
        ///
        /// **Never longer than `sFrameSlots`, because that is how long a report stays true.**
        /// `FrameResult::mGpu` is a span into the frame's own timer and the slot resolves again
        /// when it comes round, so a report held past that would carry another frame's zones.
        /// `finishOldest` drops the oldest rather than let that happen, and a caller asking once a
        /// frame never gets near it. A new world drops what is left, and `setScene` says why.
        std::vector<FrameResult> mReports;
    };
}
