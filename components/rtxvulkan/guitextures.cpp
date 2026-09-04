#include "guitextures.hpp"

#include <cassert>
#include <cstring>
#include <utility>

#include "commands.hpp"
#include "device.hpp"
#include "graveyard.hpp"
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

    std::span<std::uint8_t> GuiTextures::lend(std::uint32_t slot, const Renderer::GuiRegion& region)
    {
        assert(mLentSlot == sNothingLent && "a second lend before the first was sent");
        assert(holds(slot) && "a write to a slot nothing holds");
        assert(region.mX + region.mWidth <= mImages[slot]->getWidth()
            && region.mY + region.mHeight <= mImages[slot]->getHeight() && "a region past the edge of the texture");

        const VkDeviceSize bytes = VkDeviceSize{ region.mWidth } * region.mHeight * 4;

        // Before the lend is on the books, because a run that does not fit submits what is already
        // recorded and waits for it — and nothing may be lent across that.
        mLentAt = reserve(bytes);

        mLentSlot = slot;
        mLentRegion = region;

        return mStaging[mArena].writable<std::uint8_t>(mLentAt, bytes);
    }

    void GuiTextures::send(std::uint32_t slot)
    {
        assert(mLentSlot == slot && "a send of a slot nothing was lent for");

        const Renderer::GuiRegion region = mLentRegion;
        mLentSlot = sNothingLent;

        if (region.mWidth == 0 || region.mHeight == 0)
            return;

        // **The two transitions are what order this against the write before it.** Copies into one
        // image are otherwise unordered within a submit, and a picture written twice in a frame —
        // the whole surface and then a corner of it — would land in whichever order the device
        // chose. The barriers chain: this copy's leading barrier waits on the sampling stage the
        // previous copy's trailing barrier released to.
        const Image& image = *mImages[slot];
        const VkCommandBuffer commands = mBatch.getCommands();

        image.transition(commands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);

        const VkBufferImageCopy copy{
            .bufferOffset = mLentAt,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { static_cast<std::int32_t>(region.mX), static_cast<std::int32_t>(region.mY), 0 },
            .imageExtent = { region.mWidth, region.mHeight, 1 },
        };
        vkCmdCopyBufferToImage(
            commands, mStaging[mArena].getHandle(), image.getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        image.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void GuiTextures::write(std::uint32_t slot, const Renderer::GuiRegion& region, std::span<const std::uint8_t> rgba)
    {
        const std::span<std::uint8_t> into = lend(slot, region);
        assert(rgba.size() == into.size() && "the region's own rows, four bytes a pixel, tightly packed");

        std::memcpy(into.data(), rgba.data(), into.size());
        send(slot);
    }

    VkDeviceSize GuiTextures::reserve(VkDeviceSize bytes)
    {
        Buffer& arena = mStaging[mArena];
        VkDeviceSize at = (mStagingUsed + sCopyAlignment - 1) & ~(sCopyAlignment - 1);

        if (at + bytes > arena.getSize())
        {
            // **Waited for before the arena is rewound or replaced, and that is the whole of the
            // safety here.** What was recorded reads these bytes; handing it over would only order
            // it, and this is the one place that needs it to have run.
            finish();
            at = 0;

            if (bytes > arena.getSize())
                arena = Buffer::hostWritten(mDevice, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        }

        mStagingUsed = at + bytes;

        return at;
    }

    void GuiTextures::handOver()
    {
        assert(mLentSlot == sNothingLent && "a hand over with a lend outstanding");

        mBatch.defer();
    }

    void GuiTextures::finish()
    {
        assert(mLentSlot == sNothingLent && "a finish with a lend outstanding");

        // Two calls, because the batch's own submit carries what was handed over before it only
        // when there is something left in the batch to submit.
        mBatch.flush();
        mPool.finishDeferred();

        mStagingUsed = 0;
    }

    void GuiTextures::startFrame(Graveyard& kept)
    {
        assert(mLentSlot == sNothingLent && "an interface frame that began with a lend outstanding");

        for (std::unique_ptr<Image>& image : mRetired)
            kept.bury(std::move(image));

        mRetired.clear();

        mArena = (mArena + 1) % sStagingArenas;
        mStagingUsed = 0;
    }

    void GuiTextures::drop(std::uint32_t slot)
    {
        assert(slot < mImages.size() && mImages[slot] != nullptr && "a slot given back twice");

        // **Put aside rather than destroyed, so giving a texture back costs no submit.** A clear or
        // a copy recorded against this image has not run yet, and destroying it under a recorded
        // command is a use after free; flushing here instead would put a round trip on every window
        // that closes, and a load closes a great many. What was drawn with it is on the queue too,
        // which is why the wait that frees it is a frame's and not this class's — see `startFrame`.
        mRetired.push_back(std::move(mImages[slot]));
        mFree.push_back(slot);
    }

    void GuiTextures::read(std::uint32_t slot, std::vector<std::uint8_t>& pixels)
    {
        assert(slot < mImages.size() && mImages[slot] != nullptr && "a read of a slot nothing holds");

        // The read back submits and waits for itself, and carries what is handed over here ahead of
        // its own copy — so the bytes it takes off the device are the ones just written.
        handOver();

        mImages[slot]->read(mPool, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pixels);
    }

    VkImageView GuiTextures::getView(std::uint32_t slot)
    {
        handOver();

        if (slot >= mImages.size() || mImages[slot] == nullptr)
            return VK_NULL_HANDLE;

        return mImages[slot]->getView();
    }
}
