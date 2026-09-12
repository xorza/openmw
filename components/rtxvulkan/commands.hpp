#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Device;
    class Image;
    class Graveyard;

    /// The one command pool, and both ways a submit is made out of it: a load asks the queue and
    /// waits, and a frame cannot wait, because a frame that drained the queue could not hand the
    /// CPU the next one to walk.
    class CommandPool
    {
    public:
        explicit CommandPool(const Device& device);

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

        /// Command buffers the caller records into again every frame. The pool allows individual
        /// reset, so re-recording one is `vkBeginCommandBuffer` and nothing else. They live as long
        /// as the pool does and are not freed individually.
        std::vector<VkCommandBuffer> allocate(std::uint32_t count);

        /// Begins one of them, one-shot like everything this pool hands out.
        void begin(VkCommandBuffer commands);

        /// Frees every buffer this pool has handed out, and forgets what they referenced: a
        /// recorded buffer keeps its resources alive as far as the layers are concerned, so an image
        /// a resize destroys cannot still be named by the recording that blitted from it. Nothing
        /// may be in flight and nothing deferred.
        void reset();

        /// Takes a recorded batch to submit ahead of the next submit this pool makes, and holds its
        /// staging until that submit has been waited on — what lets an arrival ride the placement
        /// that follows it instead of costing a round trip of its own. Ends `commands`.
        void defer(VkCommandBuffer commands, std::vector<Buffer>&& staging);

        /// Submits whatever was deferred and waits for it, for the two paths that take the pool
        /// apart — a resize and shutdown — which have no next submit to give a deferred batch.
        void finishDeferred();

        /// Submits `commands` behind whatever was deferred, signalling `fence` where one is given,
        /// and does not wait — the frame's own submit. Ends `commands`. What the deferred batches
        /// read from goes to `kept`, to be let go when the caller knows the queue has passed it.
        void submit(VkCommandBuffer commands, VkFence fence, Graveyard& kept);

        /// Frees one-shot command buffers this pool handed out and the queue has finished with.
        void free(std::span<const VkCommandBuffer> commands);

    private:
        friend class Batch;

        VkCommandBuffer begin();
        void endAndWait(VkCommandBuffer commands);

        /// Gives back a recording nobody will submit. `Batch::~Batch` says when that happens.
        void discard(VkCommandBuffer commands);

        /// Submits every deferred batch and then `commands`, as one submit signalling `fence`. A
        /// deferred batch ends every upload and every build in a barrier, so what `commands` reads
        /// of them is what it would have read had they been recorded into it.
        void submitWithDeferred(VkCommandBuffer commands, VkFence fence);

        /// Lets go of what was deferred, once it has been submitted and whoever wanted its staging
        /// has taken it.
        void forgetDeferred();

        const Device& mDevice;
        Owned<VkCommandPool, vkDestroyCommandPool> mHandle;
        Owned<VkFence, vkDestroyFence> mFence;

        /// Recorded and ended, waiting for the next submit to carry them first, with the staging
        /// their copies read.
        std::vector<VkCommandBuffer> mDeferred;
        std::vector<Buffer> mDeferredStaging;

        /// Refilled per submit: a frame is three of them, and none allocates.
        std::vector<VkCommandBufferSubmitInfo> mSubmitScratch;
    };

    /// How much staging a batch takes at a time, sized so a town's tens of megabytes of textures
    /// cost a few blocks rather than hundreds of buffers. An upload larger than a block is given a
    /// block of its own exactly its size.
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
    /// a submit each is 367 waits on a queue that could have been asked once. The batch owns the
    /// staging, because the copy has not run when an upload returns. What is recorded is readable
    /// by what is recorded after it — `uploadBuffer` ends in a barrier, and a `Texture` leaves its
    /// image in `SHADER_READ_ONLY_OPTIMAL` — and nothing else here orders anything.
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

        /// Holds `staging` until this batch has been submitted and waited on.
        void keep(Buffer&& staging);

        /// Writes `bytes` into the batch's own staging and says where they landed. One block serves
        /// every upload of a batch, where a buffer apiece was three driver calls per upload and a
        /// cell uploads four hundred times. Appended and never rewound, because nothing has run yet.
        StagingRun stage(const Device& device, std::span<const std::byte> bytes);

        /// Submits what has been recorded and waits for it, then releases the staging. Does nothing
        /// where nothing was recorded, so a batch nobody used costs nothing.
        void flush();

        /// Hands what has been recorded to the pool, staging and all, to go ahead of the pool's next
        /// submit; records nothing more. The other way out of a batch, for a load that is followed
        /// by a submit anyway.
        void defer();

    private:
        /// Lets go of everything this batch was holding, whichever way it ended.
        void release();

        CommandPool& mPool;
        VkCommandBuffer mCommands = VK_NULL_HANDLE;

        /// What callers handed over with `keep`, which is whole buffers of their own making.
        std::vector<Buffer> mStaging;

        /// The batch's own staging, and how much of the last block is spoken for. See `stage`.
        std::vector<Buffer> mBlocks;
        VkDeviceSize mFilled = 0;
    };

    /// Stages `bytes` through the batch's own staging and copies them into `into` at `offset`.
    /// Nothing is ordered here: a run of these is made readable together by `orderStagedWrites`.
    void stageInto(
        Batch& batch, const Device& device, const Buffer& into, VkDeviceSize offset, std::span<const std::byte> bytes);

    /// A device-local buffer holding `bytes`, staged through host-visible memory. The copy is
    /// recorded into `batch` and ends in a barrier, so a structure can be built from it in the same
    /// batch.
    Buffer uploadBuffer(const Device& device, Batch& batch, std::span<const std::byte> bytes, VkBufferUsageFlags usage);

    /// Makes every staged write recorded into `batch` visible to whatever reads it next — one
    /// dependency for a run of writes that are read together, rather than one barrier per buffer.
    void orderStagedWrites(Batch& batch);

    /// Copies `bytes` into `image` by `regions`, and leaves it where a sampler expects it. From
    /// `UNDEFINED`, so whatever the image held is thrown away; an image written over in part —
    /// the interface's textures — transitions from where it stands instead, in `GuiTextures`.
    /// `regions` is written to: each is moved along by where the bytes landed in the staging.
    void uploadImage(const Device& device, Batch& batch, Image& image, std::span<const std::byte> bytes,
        std::span<VkBufferImageCopy> regions);

    template <class T>
    Buffer uploadBuffer(const Device& device, Batch& batch, std::span<const T> data, VkBufferUsageFlags usage)
    {
        return uploadBuffer(device, batch, std::as_bytes(data), usage);
    }
}
