#include "guitextures.hpp"

#include <cassert>

#include "commands.hpp"
#include "device.hpp"
#include "image.hpp"

namespace Rtx
{
    namespace
    {
        /// What `vkCmdCopyBufferToImage` requires of a buffer offset: a multiple of four and of the
        /// texel block, and every texture here is four bytes a texel.
        constexpr VkDeviceSize sCopyAlignment = 4;
    }

    GuiTextures::GuiTextures(const Device& device, CommandPool& pool)
        : mDevice(device)
        , mPool(pool)
        , mBatch(pool)
    {
    }

    GuiTextures::~GuiTextures() = default;

    std::uint32_t GuiTextures::add(std::uint32_t width, std::uint32_t height)
    {
        auto image = std::make_unique<Image>(mDevice, width, height, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            "gui texture");

        // **Cleared rather than left undefined.** A slot is sampleable from the moment anything can
        // observe it, so a batch drawn before the first write shows nothing instead of whatever the
        // memory held — and the pass never has to ask whether a texture is ready.
        const VkCommandBuffer commands = mBatch.getCommands();

        image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        const VkClearColorValue clear{};
        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(commands, image->getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &whole);

        image->transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        if (!mFree.empty())
        {
            const std::uint32_t slot = mFree.back();
            mFree.pop_back();
            mImages[slot] = std::move(image);
            return slot;
        }

        mImages.push_back(std::move(image));
        return static_cast<std::uint32_t>(mImages.size() - 1);
    }

    void GuiTextures::write(std::uint32_t slot, const Renderer::GuiRegion& region, std::span<const std::uint8_t> rgba)
    {
        assert(slot < mImages.size() && mImages[slot] != nullptr && "a write to a slot nothing holds");

        const Image& image = *mImages[slot];
        assert(region.mX + region.mWidth <= image.getWidth() && region.mY + region.mHeight <= image.getHeight()
            && "a region past the edge of the texture");

        const VkDeviceSize bytes = VkDeviceSize{ region.mWidth } * region.mHeight * 4;
        assert(rgba.size() == bytes && "the region's own rows, four bytes a pixel, tightly packed");

        if (bytes == 0)
            return;

        // Before the recording, because a staging run that does not fit flushes what is already
        // there — and what is already there names this image.
        const VkDeviceSize at = stage(rgba);

        // **The two transitions are what order this against the write before it.** Copies into one
        // image are otherwise unordered within a submit, and a picture written twice in a frame —
        // the whole surface and then a corner of it — would land in whichever order the device
        // chose. The barriers chain: this copy's leading barrier waits on the sampling stage the
        // previous copy's trailing barrier released to.
        const VkCommandBuffer commands = mBatch.getCommands();

        image.transition(commands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);

        const VkBufferImageCopy copy{
            .bufferOffset = at,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { static_cast<std::int32_t>(region.mX), static_cast<std::int32_t>(region.mY), 0 },
            .imageExtent = { region.mWidth, region.mHeight, 1 },
        };
        vkCmdCopyBufferToImage(
            commands, mStaging.getHandle(), image.getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        image.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    VkDeviceSize GuiTextures::stage(std::span<const std::uint8_t> rgba)
    {
        const VkDeviceSize bytes = rgba.size();
        VkDeviceSize at = (mStagingUsed + sCopyAlignment - 1) & ~(sCopyAlignment - 1);

        if (at + bytes > mStaging.getSize())
        {
            // **Flushed before the buffer is replaced, and that is the whole of the safety here.**
            // What was recorded reads out of the buffer being let go, and the flush is what makes
            // those copies have run.
            flush();
            at = 0;

            if (bytes > mStaging.getSize())
                mStaging = HostBuffer(mDevice, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        }

        mStaging.writeAt(at, rgba);
        mStagingUsed = at + bytes;

        return at;
    }

    void GuiTextures::flush()
    {
        mBatch.flush();
        mStagingUsed = 0;

        // After the flush and not before it: what was recorded names these, and the flush is where
        // it stops.
        mRetired.clear();
    }

    void GuiTextures::drop(std::uint32_t slot)
    {
        assert(slot < mImages.size() && mImages[slot] != nullptr && "a slot given back twice");

        // **Put aside rather than destroyed, so giving a texture back costs no submit.** A clear or
        // a copy recorded against this image has not run yet, and destroying it under a recorded
        // command is a use after free; flushing here instead would put a round trip on every window
        // that closes, and a load closes a great many. Nothing else is in flight to hold it: every
        // submit here waits, and so does the one that drew with it.
        mRetired.push_back(std::move(mImages[slot]));
        mFree.push_back(slot);
    }

    void GuiTextures::read(std::uint32_t slot, std::vector<std::uint8_t>& pixels)
    {
        assert(slot < mImages.size() && mImages[slot] != nullptr && "a read of a slot nothing holds");

        flush();

        mImages[slot]->read(mPool, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pixels);
    }

    VkImageView GuiTextures::getView(std::uint32_t slot)
    {
        flush();

        if (slot >= mImages.size() || mImages[slot] == nullptr)
            return VK_NULL_HANDLE;

        return mImages[slot]->getView();
    }
}
