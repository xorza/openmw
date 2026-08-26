#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>

#include "commands.hpp"
#include "hostbuffer.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;

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
    /// **Nothing here submits on its own.** Making a texture and writing one are recorded into a
    /// batch and submitted together, because a submit is a round trip to the queue and the interface
    /// makes a texture per picture widget, per font atlas and per traced view. What decides when
    /// that happens is the reader: every accessor below flushes first, so the work is done by the
    /// time anything can observe it and there is no ordering rule for a caller to remember.
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

        /// A rectangle of a texture, four bytes a pixel, tightly packed, row zero first.
        ///
        /// `rgba` is the region's own rows and not slices of a wider image, which is what lets the
        /// staging buffer hold the region rather than the surface. The rectangle must lie inside the
        /// texture, which is a contract and so an assert.
        ///
        /// The bytes are copied into the staging buffer here and the copy is recorded; neither has
        /// reached the texture when this returns, so `rgba` need not outlive the call.
        void write(std::uint32_t slot, const Renderer::GuiRegion& region, std::span<const std::uint8_t> rgba);

        void drop(std::uint32_t slot);

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
            // here writes this image and reaches the queue ahead of anything not yet submitted.
            flush();

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

    private:
        /// Submits what has been recorded and waits for it, then takes back the staging and lets go
        /// of the textures that were given back while it was pending.
        ///
        /// Costs nothing where nothing is pending, which is what lets every accessor call it.
        void flush();

        /// Copies `rgba` into the staging buffer and says where it landed.
        ///
        /// **A run at a time out of one buffer**, so several writes can share a submit: a single
        /// buffer rewritten from the start would have the second write overwrite the first's bytes
        /// before either copy had run.
        VkDeviceSize stage(std::span<const std::uint8_t> rgba);

        const Device& mDevice;
        CommandPool& mPool;

        std::vector<std::unique_ptr<Image>> mImages;
        std::vector<std::uint32_t> mFree;

        /// Grown to the largest single region ever written and reused after that: a video frame
        /// arrives through here whole once a frame and must not allocate to do it.
        ///
        /// **Sized to a region rather than to a frame's worth of them.** A frame that writes more
        /// than this holds flushes partway through and carries on, which costs a submit and bounds
        /// what the staging can grow to; sizing it to the largest frame instead would hold a load's
        /// worth of textures in host-visible video memory for the rest of the session.
        HostBuffer mStaging;
        VkDeviceSize mStagingUsed = 0;

        /// Textures given back, held until the batch that names them has been submitted.
        std::vector<std::unique_ptr<Image>> mRetired;

        /// Last, so that it is destroyed first: its own destructor flushes, and what it has
        /// recorded names images, retired images and staging that must still exist when that
        /// happens.
        Batch mBatch;
    };
}
