#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "hostbuffer.hpp"

namespace Rtx
{
    class Device;
    class Graveyard;

    /// A command pool and the one-shot submit that setup work is made of.
    ///
    /// Setup only. Recording a frame means reusing a buffer against a fence, not allocating one and
    /// waiting for the queue to drain, and nothing here is shaped for that.
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
        void defer(VkCommandBuffer commands, std::vector<Buffer>&& staging, std::vector<HostBuffer>&& hostStaging);

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
        std::vector<HostBuffer> mDeferredHostStaging;

        /// Refilled per submit: a frame is three of them, and none allocates.
        std::vector<VkCommandBufferSubmitInfo> mSubmitScratch;
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

        /// Flushes. Failing to submit is logged rather than thrown: a destructor cannot let one out,
        /// and a caller that wants to handle it calls `flush` itself.
        ~Batch();

        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;

        /// What to record into. Opens a command buffer on first use, and again after a flush.
        VkCommandBuffer getCommands();

        /// Holds `staging` until this batch has been submitted and waited on.
        void keep(Buffer&& staging);

        /// The same for a buffer the host wrote into directly, which is what a build input the
        /// device reads once and never again is: written before the submit, read inside it, and of
        /// no use to anyone afterwards.
        void keep(HostBuffer&& staging);

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
        CommandPool& mPool;
        VkCommandBuffer mCommands = VK_NULL_HANDLE;
        std::vector<Buffer> mStaging;
        std::vector<HostBuffer> mHostStaging;
    };

    /// A device-local buffer holding `bytes`, staged through host-visible memory.
    ///
    /// The copy is recorded into `batch` and the staging left in its keeping, so nothing has run
    /// when this returns. A barrier after the copy makes the result readable by anything recorded
    /// later in the same batch, which is what lets a structure be built from a buffer uploaded
    /// beside it.
    Buffer uploadBuffer(const Device& device, Batch& batch, std::span<const std::byte> bytes, VkBufferUsageFlags usage);

    template <class T>
    Buffer uploadBuffer(const Device& device, Batch& batch, std::span<const T> data, VkBufferUsageFlags usage)
    {
        return uploadBuffer(device, batch, std::as_bytes(data), usage);
    }
}
