#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"

namespace Rtx
{
    class Device;
    class Image;
    class Graveyard;

    /// The one command pool, and both ways a submit is made out of it.
    ///
    /// **Setup and the frame path both, which is why there are two.** A load asks the queue once per
    /// resource and waits for it; a frame cannot wait, because a frame that drained the queue could
    /// not hand the CPU the next one to walk. What each half costs is on the members themselves.
    class CommandPool
    {
    public:
        explicit CommandPool(const Device& device);
        ~CommandPool();

        CommandPool(const CommandPool&) = delete;
        CommandPool& operator=(const CommandPool&) = delete;

        /// Records `record` into a fresh command buffer, submits it, and waits for the queue.
        ///
        /// **For the one-off**: a resize, a read back off the device, a frame's placement. Anything
        /// that happens once per resource wants a `Batch` instead, or the queue is asked to do one
        /// thing three hundred times.
        template <class F>
        void submitAndWait(F&& record)
        {
            const VkCommandBuffer commands = begin();
            record(commands);
            endAndWait(commands);
        }

        /// Command buffers the caller records into again every frame.
        ///
        /// The pool allows individual reset, so re-recording one is `vkBeginCommandBuffer` and
        /// nothing else. They live as long as the pool does and are not freed individually.
        std::vector<VkCommandBuffer> allocate(std::uint32_t count);

        /// Begins one of them, one-shot like everything this pool hands out.
        void begin(VkCommandBuffer commands);

        /// Frees every buffer this pool has handed out, and forgets what they referenced.
        ///
        /// **A recorded buffer keeps its resources alive as far as the layers are concerned**, so an
        /// image one of them blitted from cannot be destroyed while the recording still names it —
        /// which is exactly what a resize does to the frame the last present read. Every handle from
        /// `allocate` becomes invalid, and nothing may be in flight: the caller has waited, and
        /// nothing is deferred.
        void reset();

        /// Takes a recorded batch to submit ahead of the next submit this pool makes, and holds its
        /// staging until that submit has been waited on.
        ///
        /// **What lets an arrival ride the placement that follows it.** A cell's textures and
        /// structures used to be a submit and a fence wait of their own, before the placement
        /// submitted and waited again; taken here they go to the queue in that same call, ordered
        /// ahead of it by the barriers each upload and build ends in, and the round trip they cost
        /// is the one the placement was already paying. Ends `commands`.
        void defer(VkCommandBuffer commands, std::vector<Buffer>&& staging);

        /// Submits whatever was deferred and waits for it. Does nothing where nothing was deferred.
        ///
        /// **What a deferred batch has no other way out of.** It rides the pool's next submit, and
        /// the two paths that take the pool apart — a resize and shutdown — are the two that have no
        /// next submit to give it. `reset` says so as an assert.
        void finishDeferred();

        /// Submits `commands` behind whatever was deferred, signalling `fence` where one is given,
        /// and does not wait. Ends `commands`. What the deferred batches read from goes to `kept`,
        /// to be let go when the caller knows the queue has passed it.
        ///
        /// **The frame's own submit.** `submitAndWait` is for the one-off; a frame that waited on
        /// its own trace could not hand the CPU the next frame's walk to do meanwhile, which is the
        /// whole of what two frames in flight buys.
        void submit(VkCommandBuffer commands, VkFence fence, Graveyard& kept);

        /// Frees one-shot command buffers this pool handed out and the queue has finished with.
        void free(std::span<const VkCommandBuffer> commands);

    private:
        friend class Batch;

        VkCommandBuffer begin();
        void endAndWait(VkCommandBuffer commands);

        /// Gives back a recording nobody will submit. `Batch::~Batch` says when that happens.
        void discard(VkCommandBuffer commands);

        /// Submits every deferred batch and then `commands`, as one submit signalling `fence`.
        ///
        /// **In that order and in one call, which is the whole of what deferring is for.** Command
        /// buffers in a submit run in order as far as the barriers between them say, and a deferred
        /// batch ends every upload and every build in one — so what `commands` reads of them is
        /// what it would have read had they been recorded into it.
        void submitWithDeferred(VkCommandBuffer commands, VkFence fence);

        /// Lets go of what was deferred, once it has been submitted and whoever wanted its staging
        /// has taken it.
        void forgetDeferred();

        const Device& mDevice;
        VkCommandPool mHandle = VK_NULL_HANDLE;
        VkFence mFence = VK_NULL_HANDLE;

        /// Recorded and ended, waiting for the next submit to carry them first, with the staging
        /// their copies read.
        std::vector<VkCommandBuffer> mDeferred;
        std::vector<Buffer> mDeferredStaging;

        /// Refilled per submit: a frame is three of them, and none allocates.
        std::vector<VkCommandBufferSubmitInfo> mSubmitScratch;
    };

    /// How much staging a batch takes at a time.
    ///
    /// **Sized so a cell's textures cost a handful of allocations and not one apiece.** Balmora
    /// arrives with sixty-odd megabytes of them; at this size that is a few blocks where it was four
    /// hundred buffers. An upload larger than a block is given a block of its own exactly its size,
    /// so nothing is rounded up to this that does not need to be.
    inline constexpr VkDeviceSize sStagingBlock = 8 * 1024 * 1024;

    /// What every run inside a block starts on.
    ///
    /// **The largest texel block of any format this renderer uploads**, which is BC2's and BC3's
    /// sixteen bytes. `VkBufferImageCopy::bufferOffset` has to be a multiple of four and of the
    /// format's texel block, and a texture's own level offsets are added to a run's start — so a
    /// start every requirement divides leaves every sum as legal as it was.
    inline constexpr VkDeviceSize sStagingAlignment = 16;

    /// Where a batch put an upload's bytes: the buffer holding them, and how far into it they
    /// start. See `Batch::stage`.
    struct StagingRun
    {
        VkBuffer mBuffer = VK_NULL_HANDLE;
        VkDeviceSize mOffset = 0;
    };

    /// One command buffer that a run of setup records into, submitted and waited on once.
    ///
    /// **A load path's cost is round trips, not work.** A cell arriving at Balmora creates 361
    /// textures and half a dozen buffers, and a submit each means 367 waits on a queue that could
    /// have been asked once. The work is identical; what goes is the driver and the fence between
    /// every piece of it.
    ///
    /// **The batch owns the staging.** A single upload keeps its staging buffer as a local and
    /// relies on the wait happening before it goes out of scope. Recorded into a batch, the copy has
    /// not run yet, so the staging is handed here with `keep` and destroyed when the flush returns.
    ///
    /// **What is recorded is readable by what is recorded after it**, so an upload can be built on
    /// in the same batch: `uploadBuffer` ends in a barrier, and a `Texture` leaves its image in
    /// `SHADER_READ_ONLY_OPTIMAL`. Nothing else here orders anything.
    ///
    /// **This is not asynchrony and does not stand in its way.** The flush still waits; what it
    /// stops is asking three hundred times. Deferred, the batch does not wait at all: the next
    /// submit carries it and waits for both.
    class Batch
    {
    public:
        explicit Batch(CommandPool& pool)
            : mPool(pool)
        {
        }

        /// Throws away anything still recorded.
        ///
        /// **A destructor is not where a submit belongs.** It runs during unwinding too, and a
        /// constructor that fails half way leaves a recording naming resources its own members have
        /// already let go of: `Texture` records the upload of its primary image, the shading image's
        /// allocation throws, and the image is destroyed before a submit here would carry the copy
        /// that names it. So a batch that leaves without `flush` or `defer` submits nothing.
        ///
        /// **Unwinding is the case this exists for**, and a caller that simply forgot is a contract
        /// broken — which is the one an assert names, because the other is not a mistake.
        ~Batch();

        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;

        /// What to record into. Opens a command buffer on first use, and again after a flush.
        VkCommandBuffer getCommands();

        /// Holds `staging` until this batch has been submitted and waited on.
        void keep(Buffer&& staging);

        /// Writes `bytes` into the batch's own staging and says where they landed.
        ///
        /// **One block serves every upload of a batch.** A buffer apiece was a `vkCreateBuffer`, a
        /// `vkAllocateMemory` and a `vkMapMemory` per upload, and a cell that arrives with two
        /// hundred textures uploads four hundred times — the levels and the shading map of each.
        /// The bytes are the same bytes and they are alive just as long either way: the batch has
        /// held every one of them until the submit since it was written.
        ///
        /// **Appended and never rewound**, because nothing has run yet: a block reused inside one
        /// batch would have the copy of the upload before reading whatever the one after wrote.
        /// A block that cannot take an upload is left where it is and another is taken.
        StagingRun stage(const Device& device, std::span<const std::byte> bytes);

        /// Submits what has been recorded and waits for it, then releases the staging. Does nothing
        /// where nothing was recorded, so a batch nobody used costs nothing.
        void flush();

        /// Hands what has been recorded to the pool, staging and all, to go ahead of the pool's next
        /// submit; records nothing more. Does nothing where nothing was recorded.
        ///
        /// **The other way out of a batch**, for a load that is followed by a submit anyway: `flush`
        /// asks the queue now and waits, and this lets the placement that follows ask once for both.
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

    /// A device-local buffer holding `bytes`, staged through host-visible memory.
    ///
    /// The copy is recorded into `batch` and the staging left in its keeping, so nothing has run
    /// when this returns. A barrier after the copy makes the result readable by anything recorded
    /// later in the same batch, which is what lets a structure be built from a buffer uploaded
    /// beside it.
    Buffer uploadBuffer(const Device& device, Batch& batch, std::span<const std::byte> bytes, VkBufferUsageFlags usage);

    /// Makes every staged write recorded into `batch` visible to whatever reads it next.
    ///
    /// **One dependency for a run of writes, because they are read together.** A load stages a
    /// mesh's vertices, indices, normals and texture coordinates into the same command buffer that
    /// then builds an acceleration structure out of them, so the copies are ordered against the
    /// build once rather than one barrier per buffer. `uploadBuffer` is the other shape: a single
    /// buffer whose reader may be the very next command.
    void orderStagedWrites(Batch& batch);

    /// Copies `bytes` into `image` by `regions`, and leaves it where a sampler expects it.
    ///
    /// **Recorded rather than submitted.** A cell brings hundreds of these and the queue is asked
    /// once for all of them; the image is left where a sampler expects it, so nothing recorded
    /// afterwards has to know this one happened.
    ///
    /// **From `UNDEFINED`, so whatever the image held is thrown away.** That is what an image being
    /// filled for the first time is. An image already in a sampler's hands that is being written
    /// over in part — the interface's textures — transitions from where it stands instead, and
    /// `GuiTextures` is that.
    ///
    /// **`regions` is written to**, because the bytes land somewhere inside the batch's staging
    /// rather than at the start of a buffer of their own: each region is moved along by where they
    /// landed.
    void uploadImage(const Device& device, Batch& batch, Image& image, std::span<const std::byte> bytes,
        std::span<VkBufferImageCopy> regions);

    template <class T>
    Buffer uploadBuffer(const Device& device, Batch& batch, std::span<const T> data, VkBufferUsageFlags usage)
    {
        return uploadBuffer(device, batch, std::as_bytes(data), usage);
    }
}
