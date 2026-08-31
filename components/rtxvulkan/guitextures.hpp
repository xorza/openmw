#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>

#include "buffer.hpp"
#include "commands.hpp"
#include "frameslots.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;
    class Graveyard;

    /// Every texture the GUI draws with, addressed by slot.
    ///
    /// **Not the scene's bindless array, and deliberately nothing like it.** That one is indexed by
    /// material, sized to the world and appended to when a cell arrives; these are a font atlas, a
    /// skin sheet, a map and a video frame — a handful of images with nothing to do with what is
    /// being traced, arriving and leaving as windows open and close.
    ///
    /// **A slot a texture gave back is taken over before the table grows**, so a session of opening
    /// and closing menus does not walk the table upwards forever.
    ///
    /// **Nothing here waits on the frame path.** Making a texture and writing one are recorded into
    /// a batch and handed to the pool, to go ahead of whatever submits next — the interface's own
    /// draw, a picture traced into a slot, a read back. Every reader needs these copies *ordered*
    /// before it, which a handed-over batch is; having them *finished* is what a submit and a fence
    /// of this class's own would buy, and nobody asked for it. It is not cheap either: the queue is
    /// one queue and the frame went onto it a moment ago, so waiting here is waiting for the whole
    /// traced frame, on every frame that wrote a texture at all. `finish` is the exception and says
    /// what it is for.
    ///
    /// **Nothing outside reaches an image, and that is what makes the layout sayable.** A texture
    /// rests in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` between the calls here, which is a rule
    /// only worth stating if the one path that writes a texture with device commands — a traced
    /// view — goes through `writeWith` rather than transitioning it by hand.
    class GuiTextures
    {
    public:
        GuiTextures(const Device& device, CommandPool& pool);
        ~GuiTextures();

        GuiTextures(const GuiTextures&) = delete;
        GuiTextures& operator=(const GuiTextures&) = delete;

        /// A slot holding a texture of this size, cleared to nothing.
        std::uint32_t add(std::uint32_t width, std::uint32_t height);

        /// Bytes for a rectangle of a texture, to be filled and then handed back with `send`.
        ///
        /// **The memory the copy will read, so that filling it is the only time the pixels are
        /// written.** MyGUI's own interface is a buffer lent out and taken back filled, and a
        /// backend that lends one of its own puts a copy in front of every write — a video frame
        /// then crosses main memory twice on its way to a device that could have been written into
        /// once.
        ///
        /// The rectangle must lie inside the texture, and only one may be lent at a time. Both are
        /// contracts and so asserts. The span is `height` rows of `width` pixels, four bytes each,
        /// tightly packed, row zero first; it stops being writable at `send`.
        ///
        /// **Write it and do not read it back.** This is host-visible device memory, which is write
        /// combined: filling it in order costs what a copy into main memory costs, and reading a
        /// byte of it back costs far more than either.
        std::span<std::uint8_t> lend(std::uint32_t slot, const Renderer::GuiRegion& region);

        /// Records the copy of what `lend` handed out. Nothing has run when this returns.
        void send(std::uint32_t slot);

        /// A rectangle of a texture, four bytes a pixel, tightly packed, row zero first.
        ///
        /// `rgba` is the region's own rows and not slices of a wider image. For a caller that
        /// already holds the pixels; one that is about to produce them wants `lend` instead, and
        /// this is that pair with a copy in front of it.
        void write(std::uint32_t slot, const Renderer::GuiRegion& region, std::span<const std::uint8_t> rgba);

        void drop(std::uint32_t slot);

        /// Opens an interface frame: hands `kept` every texture given back since the last one, and
        /// takes the staging that frame's fence has just freed.
        ///
        /// **A texture is given back a frame after it was last drawn with, and that draw is still on
        /// the queue.** The interface is submitted without a wait and its fence is read two frames
        /// later, so nothing here can say when a view stops being read — the graveyard of the frame
        /// being recorded is what knows, exactly as it does for everything else on the frame path.
        ///
        /// **Called once per interface frame, after that frame's fence and before anything is
        /// handed over.** The staging turns on the same signal, and `mStaging` says why.
        void startFrame(Graveyard& kept);

        /// What the pass samples, or null where nothing holds that slot.
        VkImageView getView(std::uint32_t slot);

        /// Whether anything is in that slot.
        bool holds(std::uint32_t slot) const { return slot < mImages.size() && mImages[slot] != nullptr; }

        /// Lends the texture in `slot` to a caller that writes it with transfer commands, rather
        /// than by handing over pixels: `record(image, layout)` is called with it ready to be
        /// written and the layout it is in.
        ///
        /// **The layout is this class's and not its caller's.** A texture rests in
        /// `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` and is put back there, and the scope opened
        /// around what is recorded is every transfer stage — which is what *written with transfer
        /// commands* means. A scope named instead for the commands one caller happens to record has
        /// to be revisited every time that caller changes, and one branch of that agreement was
        /// missing for as long as there has been a traced view.
        ///
        /// Ordering *within* what is recorded stays the caller's: two transfer writes to one image
        /// are unordered unless something says otherwise.
        template <class Record>
        void writeWith(std::uint32_t slot, VkCommandBuffer commands, Record&& record)
        {
            // First, and whatever the caller has already recorded into `commands`: what is pending
            // here writes this image, and left in the batch it would reach the queue after the
            // buffer being recorded rather than before it.
            handOver();

            assert(holds(slot) && "a write to a slot nothing holds");

            const Image& image = *mImages[slot];

            image.transition(commands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            record(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            image.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }

        /// The whole texture in main memory, four bytes a pixel. Costs a transfer off the device.
        void read(std::uint32_t slot, std::vector<std::uint8_t>& pixels);

        /// Submits what has been recorded, and what was already handed over, and waits for both.
        ///
        /// **For the two paths that take the command pool apart**, a resize and shutdown: a batch
        /// handed over rides the pool's next submit, and those two are where there is no next
        /// submit. A staging arena that fills before its frame is over wants it for its own reason,
        /// and that is the only wait left on the frame path.
        void finish();

    private:
        /// Hands what has been recorded to the pool, to go ahead of its next submit.
        ///
        /// Costs nothing where nothing is pending, which is what lets every accessor call it.
        void handOver();

        /// A run of the current staging arena, `bytes` long, and where it starts.
        ///
        /// **A run at a time out of one arena**, so several writes can share a submit: an arena
        /// rewritten from the start would have the second write overwrite the first's bytes before
        /// either copy had run.
        VkDeviceSize reserve(VkDeviceSize bytes);

        const Device& mDevice;
        CommandPool& mPool;

        std::vector<std::unique_ptr<Image>> mImages;
        std::vector<std::uint32_t> mFree;

        /// **One more arena than there are frames in flight.**
        ///
        /// A run of an arena is being read for as long as the submit that carried it is. The batch
        /// is handed over rather than waited for, so the bytes a copy reads are still needed after
        /// the call that filled them returns — and rewinding there, which a wait used to make safe,
        /// would put the next write on top of a copy that has not run.
        ///
        /// **Three and not two, and the extra one is the whole of why this is stated.** What is
        /// written between two interface frames is carried by the *later* one's submit, and that
        /// submit's fence is waited on `sFrameSlots` frames after that — so an arena has to last
        /// from the frame before the one that sends it through to the one that waits, which is one
        /// more frame than there are slots. Two arenas hands them back exactly one frame early, and
        /// what that costs is a copy reading pixels of the frame after its own.
        static constexpr std::uint32_t sStagingArenas = sFrameSlots + 1;

        /// Written a run at a time, turned over by `startFrame`, and each grown to the largest
        /// single region ever written: a video frame arrives through here whole once a frame and
        /// must not allocate to do it.
        ///
        /// **Sized to a region rather than to a frame's worth of them.** A frame that writes more
        /// than one holds submits what is already recorded and waits for it, which costs a round
        /// trip and bounds what an arena can grow to; sizing them to the largest frame instead would
        /// hold a load's worth of textures in host-visible video memory for the rest of the session.
        std::array<Buffer, sStagingArenas> mStaging;
        std::uint32_t mArena = 0;
        VkDeviceSize mStagingUsed = 0;

        static constexpr std::uint32_t sNothingLent = ~0u;

        /// What `lend` handed bytes out of, until `send` records the copy back.
        std::uint32_t mLentSlot = sNothingLent;
        Renderer::GuiRegion mLentRegion;
        VkDeviceSize mLentAt = 0;

        /// Textures given back, held until `startFrame` hands them to a frame's graveyard.
        std::vector<std::unique_ptr<Image>> mRetired;

        /// Last, so that it is destroyed first: its own destructor flushes, and what it has
        /// recorded names images, retired images and staging that must still exist when that
        /// happens.
        Batch mBatch;
    };
}
