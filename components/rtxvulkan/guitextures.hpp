#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>
#include <components/rtx/slot.hpp>
#include <components/rtx/slots.hpp>

#include "buffer.hpp"
#include "commands.hpp"
#include "frameslots.hpp"
#include "image.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    class Device;
    class Graveyard;

    /// Every texture the GUI draws with, addressed by slot — a font atlas, a skin sheet, a map, a
    /// video frame — nothing like the scene's bindless array. A slot a texture gave back is taken
    /// over before the table grows (`Rtx::SlotPool`). Nothing here waits on the frame path: making
    /// a texture and writing one are recorded into a batch and handed to the pool, to go ahead of
    /// whatever submits next, because every reader needs these copies *ordered* before it, and
    /// waiting for them *finished* would be waiting for the whole traced frame on every frame that
    /// wrote a texture; `finish` is the exception. A texture rests in
    /// `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` between the calls here, which is why the one path
    /// that writes one with device commands goes through `writeWith`.
    class GuiTextures
    {
    public:
        GuiTextures(const Device& device, CommandPool& pool);
        ~GuiTextures();

        /// A slot holding a texture of this size, cleared to nothing.
        GuiSlot add(std::uint32_t width, std::uint32_t height);

        /// Bytes for a rectangle of a texture, to be filled and then handed back with `send` — the
        /// memory the copy will read, so that a video frame does not cross main memory twice. The
        /// rectangle must lie inside the texture, and only one may be lent at a time; both are
        /// asserts. The span is `height` rows of `width` pixels, four bytes each, tightly packed,
        /// row zero first, and stops being writable at `send`. Write it and do not read it back: it
        /// is write-combined memory.
        std::span<std::uint8_t> lend(GuiSlot slot, const GuiRegion& region);

        /// Records the copy of what `lend` handed out. Nothing has run when this returns.
        void send(GuiSlot slot);

        void drop(GuiSlot slot);

        /// Opens an interface frame: hands `kept` every texture given back since the last one, and
        /// takes the staging that frame's fence has just freed. A texture is given back a frame
        /// after it was last drawn with, and that draw is still on the queue, so the graveyard of
        /// the frame being recorded is what knows when it stops being read. Once per interface
        /// frame, after that frame's fence and before anything is handed over.
        void startFrame(Graveyard& kept);

        /// What the pass samples, or null where nothing holds that slot.
        VkImageView getView(GuiSlot slot);

        bool holds(GuiSlot slot) const
        {
            return !slot.isNone() && slot.get() < mImages.size() && mImages[slot.get()] != nullptr;
        }

        /// Lends the texture in `slot` to a caller that writes it with transfer commands:
        /// `record(image, layout)` is called with it ready to be written and the layout it is in,
        /// and the scope opened around what is recorded is every transfer stage. Ordering *within*
        /// what is recorded stays the caller's.
        template <class Record>
        void writeWith(GuiSlot slot, VkCommandBuffer commands, Record&& record)
        {
            // First, and whatever the caller has already recorded into `commands`: what is pending
            // here writes this image, and left in the batch it would reach the queue after the
            // buffer being recorded rather than before it.
            handOver();

            assert(holds(slot) && "a write to a slot nothing holds");

            const Image& image = *mImages[slot.get()];

            image.transition(commands, Use::sFragmentSample, Use::sTransferWrite);

            record(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            image.transition(commands, Use::sTransferWrite, Use::sFragmentSample);
        }

        /// The whole texture in main memory, four bytes a pixel. Costs a transfer off the device.
        void read(GuiSlot slot, std::vector<std::uint8_t>& pixels);

        /// Records a copy of the whole texture into a host-readable buffer kept for the slot, after
        /// whatever `commands` already holds, and remembers that `frame` is what carries it — into
        /// the same batch as the trace that wrote the texture, so the copy costs no submit and no
        /// wait of its own; `takeCopy` hands the bytes over once the frame has been waited for.
        ///
        /// @param graveyard where a buffer this replaces is buried, because a batch recorded against
        ///        the old one may not have run.
        void readBackWith(GuiSlot slot, VkCommandBuffer commands, std::uint64_t frame, Graveyard& graveyard);

        /// Copies what `readBackWith` left for `slot` into `into`, and answers whether it did:
        /// false until the frame carrying the copy is behind `finished`, and never a wait, because
        /// the caller asks again next frame. False too where nothing was ever asked of the slot.
        bool takeCopy(GuiSlot slot, std::span<std::uint8_t> into, std::uint64_t finished);

        /// Every copy recorded so far has run — the caller drained the queue, deferred batches and
        /// frames in flight both — so each may be taken whatever frame it was stamped with.
        void landTraces();

        /// Submits what has been recorded, and what was already handed over, and waits for both —
        /// for a resize and shutdown, where there is no next submit, and for a staging arena that
        /// fills before its frame is over, which is the only wait left on the frame path.
        void finish();

    private:
        /// Hands what has been recorded to the pool, to go ahead of its next submit. Costs nothing
        /// where nothing is pending, so every accessor can call it.
        void handOver();

        /// A run of the current staging arena, `bytes` long, and where it starts, so several writes
        /// can share a submit.
        VkDeviceSize reserve(VkDeviceSize bytes);

        const Device& mDevice;
        CommandPool& mPool;

        std::vector<std::unique_ptr<Image>> mImages;

        /// What a trace left for the host, per slot: the buffer, and the frame whose fence says
        /// it has arrived. `sNever` where nothing was asked.
        struct Copy
        {
            static constexpr std::uint64_t sNever = ~std::uint64_t{ 0 };

            std::unique_ptr<Buffer> mBuffer;
            std::uint64_t mTracedOn = sNever;
            bool mLanded = false;
        };
        std::vector<Copy> mCopies;

        /// The slots nothing holds. `SlotPool` and not a list of its own, because which free
        /// slot an arrival takes is one rule and this renderer keeps three tables by it.
        SlotPool mFree;

        /// One more arena than there are frames in flight. What is written between two interface
        /// frames is carried by the *later* one's submit, whose fence is waited on `sFrameSlots`
        /// frames after that, so an arena has to last one more frame than there are slots; two
        /// arenas hand them back exactly one frame early.
        static constexpr std::uint32_t sStagingArenas = sFrameSlots + 1;

        /// Written a run at a time, turned over by `startFrame`, and each grown to the largest
        /// single region ever written, so a video frame does not allocate. Sized to a region rather
        /// than to a frame's worth of them: a frame that writes more than one waits, which bounds
        /// what an arena can grow to.
        std::array<Buffer, sStagingArenas> mStaging;
        std::uint32_t mArena = 0;
        VkDeviceSize mStagingUsed = 0;

        /// What `lend` handed bytes out of, until `send` records the copy back.
        GuiSlot mLentSlot;
        GuiRegion mLentRegion;
        VkDeviceSize mLentAt = 0;

        /// Textures given back, held until `startFrame` hands them to a frame's graveyard.
        std::vector<std::unique_ptr<Image>> mRetired;

        /// Last, so that it is destroyed first: its own destructor flushes, and what it has
        /// recorded names images, retired images and staging that must still exist when that
        /// happens.
        Batch mBatch;
    };
}
