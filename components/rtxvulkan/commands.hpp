#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "image.hpp"
#include "owned.hpp"
#include "retiring.hpp"

namespace Rtx
{
    class Device;

    /// The one command pool, the device's own, and both ways a submit is made out of it: a load
    /// asks the queue and waits, and a frame cannot wait, because a frame that drained the queue
    /// could not hand the CPU the next one to walk.
    class CommandPool
    {
    public:
        /// Records `record` into a fresh command buffer, submits it, and waits for the queue. For
        /// the one-off; anything that happens once per resource wants a `Batch`, or the queue is
        /// asked to do one thing three hundred times.
        template <class F>
        void submitAndWait(F&& record)
        {
            const VkCommandBuffer commands = begin();
            record(commands);
            endAndWait(commands);
        }

        /// A command buffer to record into. Off the spare list where one has been given back, and
        /// allocated where none has: the pool allows individual reset, so a buffer given back is
        /// begun again with `vkBeginCommandBuffer` and nothing else, and an arrival costs no
        /// allocation once the busiest frame so far has been seen. Nothing is freed until the pool
        /// goes.
        VkCommandBuffer take();

        /// `take`, `count` times, for the buffers a ring keeps.
        std::vector<VkCommandBuffer> allocate(std::uint32_t count);

        /// Gives command buffers back for the next `take`, once the queue has finished with them.
        void recycle(std::span<const VkCommandBuffer> commands);

        /// Begins one of them, one-shot like everything this pool hands out.
        void begin(VkCommandBuffer commands);

        /// Ends a recording nobody will submit this frame — a placement that placed nothing — so
        /// the buffer can be begun again next frame. Not `discard`, which gives it back.
        void end(VkCommandBuffer commands);

        /// Takes a recorded batch to submit ahead of the next submit this pool makes — what lets an
        /// arrival ride the placement that follows it instead of costing a round trip of its own.
        /// Ends `commands`. What the batch read is the batch's to bury, under that same submit.
        void defer(VkCommandBuffer commands);

        /// Submits whatever was deferred and waits for it, for the two paths that take the pool
        /// apart — a resize and shutdown — which have no next submit to give a deferred batch.
        void finishDeferred();

        /// Submits `commands` behind whatever was deferred and does not wait — the frame's own
        /// submit. Ends `commands`. The deferred batches' command buffers retire under the value
        /// this signals, and `collect` gives them back once a wait has passed it. Returns the
        /// value the submit signals on the device's timeline, which is what says when that is.
        ///
        /// @param waits,signals binary semaphores the submit waits and signals beside the
        ///        timeline: what a present's blit needs, and what nothing else does. Through here
        ///        and not a submit of its own, because a submit that took a timeline value without
        ///        carrying the deferred batches would let the graveyard free what they name before
        ///        they run.
        std::uint64_t submit(VkCommandBuffer commands, std::span<const VkSemaphoreSubmitInfo> waits = {},
            std::span<const VkSemaphoreSubmitInfo> signals = {});

        /// What the driver's pacing knows the frame as, for every submit from here on to carry —
        /// `VkLatencySubmissionPresentIdNV` — until it is told another. Nought carries nothing,
        /// which is every submit where the driver paces nothing. The presenter's to say, after
        /// each sleep and each present; the pool knows no presenter.
        void setPresentId(std::uint64_t presentId) { mPresentId = presentId; }

        const Device& getDevice() const { return mDevice; }

        // Read by the tests and by nothing else.
        std::size_t getStagingBlockCount() const { return mStaging.size(); }

    private:
        friend class Batch;

        /// Gives back the retired command buffers the timeline has passed. The device's, after
        /// every wait, as the graveyard's `collect` is.
        friend class Device;
        void collect();

        /// Gives back every retired command buffer, for a queue nothing is on: asserted, with
        /// nothing deferred, because the same call one wait too early is a buffer begun again
        /// while the queue still executes it.
        void collectIdle();

        /// What both share: every retired buffer stamped at or below `finished` goes to the spare
        /// list.
        void releaseRetired(std::uint64_t finished);

        /// The device's alone, because the graveyard gives a finished command buffer back to the
        /// device's pool: a second pool's buffer would land in the first's spare list and outlive
        /// the pool it was allocated from.
        friend class Device;
        explicit CommandPool(const Device& device);

        VkCommandBuffer begin();
        void endAndWait(VkCommandBuffer commands);

        /// Gives back a recording nobody will submit. `Batch::~Batch` says when that happens.
        void discard(VkCommandBuffer commands);

        /// A staging block of at least `bytes` that nothing on the queue reads, made where none
        /// is free, as an index into the ring. Taken until `giveStaging`.
        std::size_t takeStaging(VkDeviceSize bytes);

        /// The block behind an index.
        const Buffer& stagingAt(std::size_t block) const { return mStaging[block].mBuffer; }

        /// Gives a block back, read until the timeline has passed `readUntil`.
        void giveStaging(std::size_t block, std::uint64_t readUntil);

        /// Submits every deferred batch and then `commands`, as one submit signalling the next
        /// value of the timeline, which it returns. A deferred batch ends every upload and every
        /// build in a barrier, so what `commands` reads of them is what it would have read had
        /// they been recorded into it.
        std::uint64_t submitWithDeferred(VkCommandBuffer commands, std::span<const VkSemaphoreSubmitInfo> waits,
            std::span<const VkSemaphoreSubmitInfo> signals);

        const Device& mDevice;
        Owned<VkCommandPool, vkDestroyCommandPool> mHandle;

        /// Recorded and ended, waiting for the next submit to carry them first.
        std::vector<VkCommandBuffer> mDeferred;

        /// Carried by a submit and not yet known to have run.
        Retiring<VkCommandBuffer> mRetiring;

        /// Given back and not yet taken again.
        std::vector<VkCommandBuffer> mSpare;

        /// The pool's staging: blocks a batch writes uploads into and copies out of, each stamped
        /// by the submit that last read it and taken again once the timeline has passed that. An
        /// arrival then costs no staging buffer of its own, and a frame with nothing arriving
        /// allocates nothing. The ring settles at the busiest stretch so far — as many blocks as
        /// the arrivals in flight together wrote — and is never shrunk.
        struct StagingBlock
        {
            Buffer mBuffer;
            std::uint64_t mReadUntil = 0;
            bool mTaken = false;
        };
        std::vector<StagingBlock> mStaging;

        /// Refilled per submit: a frame is three of them, and none allocates.
        std::vector<VkCommandBufferSubmitInfo> mSubmitScratch;
        std::vector<VkSemaphoreSubmitInfo> mSignalScratch;

        /// See `setPresentId`.
        std::uint64_t mPresentId = 0;
    };

    /// How much staging a batch takes at a time, sized so a town's tens of megabytes of textures
    /// cost a few blocks rather than hundreds of buffers. An upload larger than a block is given a
    /// block of its own exactly its size, which the ring keeps like any other.
    inline constexpr VkDeviceSize sStagingBlock = 8 * 1024 * 1024;

    /// What every run inside a block starts on: the largest texel block of any format this renderer
    /// uploads, BC2's and BC3's sixteen bytes. `VkBufferImageCopy::bufferOffset` has to be a
    /// multiple of four and of the format's texel block.
    inline constexpr VkDeviceSize sStagingAlignment = 16;

    /// Where a batch put an upload's bytes: the buffer holding them, and how far into it they
    /// start. See `Batch::stage`.
    struct StagingRun
    {
        VkBuffer mBuffer = VK_NULL_HANDLE;
        VkDeviceSize mOffset = 0;
    };

    /// One command buffer that a run of setup records into, submitted and waited on once. A load
    /// path's cost is round trips, not work: a cell arriving at Balmora creates 361 textures, and
    /// a submit each is 367 waits on a queue that could have been asked once. The batch holds the
    /// staging blocks it took off the pool's ring, because the copy has not run when an upload
    /// returns, and gives them back stamped with the submit it rides when it ends — `keep` says
    /// what else it holds until then. What is recorded is readable by what is recorded after it —
    /// `uploadBuffer` ends in a barrier, and a `Texture` leaves its image in
    /// `SHADER_READ_ONLY_OPTIMAL` — and nothing else here orders anything.
    class Batch
    {
    public:
        explicit Batch(CommandPool& pool)
            : mPool(pool)
        {
        }

        /// Throws away anything still recorded. A destructor is not where a submit belongs: it
        /// runs during unwinding too, and a constructor that fails half way leaves a recording
        /// naming resources its own members have already let go of. A caller that simply forgot
        /// `flush` or `defer` is the contract the assert names.
        ~Batch();

        /// What to record into. Opens a command buffer on first use, and again after a flush.
        VkCommandBuffer getCommands();

        /// The device the pool records for, which is the one everything staged through this is
        /// made on.
        const Device& getDevice() const { return mPool.getDevice(); }

        /// Holds what this recording reads, or what an earlier submit may still read, and buries
        /// it under the submit this batch rides when the batch ends, whichever way it ends: a
        /// buffer staged through, a build's scratch, an image the last frame drew with. Buried
        /// then and not now, because a burial is stamped with the next submit, and the submit this
        /// batch rides is not the next one until the batch is handed over.
        void keep(Buffer&& buffer);
        void keep(Image&& image);

        /// Writes `bytes` into the batch's staging and says where they landed. One block serves
        /// every upload of a batch, where a buffer apiece was three driver calls per upload and a
        /// cell uploads four hundred times. Appended and never rewound, because nothing has run yet.
        StagingRun stage(std::span<const std::byte> bytes);

        /// Buries what it held under the submit it is about to make, submits what has been
        /// recorded and waits for it. A batch nobody used costs nothing.
        void flush();

        /// Hands what has been recorded to the pool, to go ahead of the pool's next submit, and
        /// buries what it held under that submit; records nothing more. The other way out of a
        /// batch, for a load that is followed by a submit anyway.
        void defer();

    private:
        /// Buries everything this batch was holding and gives its staging back, whichever way it
        /// ended, under the next submit — which is the one this batch rides where it was handed
        /// over, and one after every reader where it was flushed or thrown away.
        void release();

        CommandPool& mPool;
        VkCommandBuffer mCommands = VK_NULL_HANDLE;

        /// What callers handed over with `keep`.
        std::vector<Buffer> mKeptBuffers;
        std::vector<Image> mKeptImages;

        /// The ring's blocks this batch took, and how much of the last one is spoken for. See
        /// `stage`.
        std::vector<std::size_t> mBlocks;
        VkDeviceSize mFilled = 0;
    };

    /// Stages `bytes` through the batch's own staging and copies them into `into` at `offset`.
    /// Nothing is ordered here: a run of these is made readable together by `orderStagedWrites`.
    void stageInto(Batch& batch, const Buffer& into, VkDeviceSize offset, std::span<const std::byte> bytes);

    /// A device-local buffer holding `bytes`, staged through the batch's staging. The copy is
    /// recorded into `batch` and ends in a barrier, so a structure can be built from it in the same
    /// batch.
    Buffer uploadBuffer(
        Batch& batch, std::span<const std::byte> bytes, VkBufferUsageFlags usage, std::string_view name);

    /// Makes every staged write recorded into `batch` visible to whatever reads it next — one
    /// dependency for a run of writes that are read together, rather than one barrier per buffer.
    void orderStagedWrites(Batch& batch);

    /// Copies `bytes` into `image` by `regions`, and leaves it where a sampler expects it. From
    /// `UNDEFINED`, so whatever the image held is thrown away; an image written over in part —
    /// the interface's textures — transitions from where it stands instead, in `GuiTextures`.
    /// `regions` is written to: each is moved along by where the bytes landed in the staging.
    void uploadImage(
        Batch& batch, Image& image, std::span<const std::byte> bytes, std::span<VkBufferImageCopy> regions);

    template <class T>
    Buffer uploadBuffer(Batch& batch, std::span<const T> data, VkBufferUsageFlags usage, std::string_view name)
    {
        return uploadBuffer(batch, std::as_bytes(data), usage, name);
    }
}
