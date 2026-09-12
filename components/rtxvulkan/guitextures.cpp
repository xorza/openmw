#include "guitextures.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <utility>

#include "commands.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "image.hpp"
#include "imageuse.hpp"

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

    GuiSlot GuiTextures::add(std::uint32_t width, std::uint32_t height)
    {
        auto image = std::make_unique<Image>(mDevice, width, height, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            "gui texture");

        // Cleared rather than left undefined. A slot is sampleable from the moment anything can
        // observe it, so a batch drawn before the first write shows nothing instead of whatever the
        // memory held — and the pass never has to ask whether a texture is ready.
        const VkCommandBuffer commands = mBatch.getCommands();

        image->transition(commands, Use::sUndefined, Use::sClearWrite);

        const VkClearColorValue clear{};
        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(commands, image->getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &whole);

        image->transition(commands, Use::sClearWrite, Use::sFragmentSample);

        if (const Index taken = mFree.take(); taken != sNoIndex)
        {
            mImages[taken] = std::move(image);
            return GuiSlot::at(taken);
        }

        mImages.push_back(std::move(image));
        mCopies.emplace_back();
        return GuiSlot::at(static_cast<std::uint32_t>(mImages.size() - 1));
    }

    std::span<std::uint8_t> GuiTextures::lend(const GuiSlot slot, const GuiRegion& region)
    {
        assert(mLentSlot.isNone() && "a second lend before the first was sent");
        assert(holds(slot) && "a write to a slot nothing holds");
        assert(region.mX + region.mWidth <= mImages[slot.get()]->getWidth()
            && region.mY + region.mHeight <= mImages[slot.get()]->getHeight()
            && "a region past the edge of the texture");

        const VkDeviceSize bytes = VkDeviceSize{ region.mWidth } * region.mHeight * 4;

        // Before the lend is on the books, because a run that does not fit submits what is already
        // recorded and waits for it — and nothing may be lent across that.
        mLentAt = reserve(bytes);

        mLentSlot = slot;
        mLentRegion = region;

        return mStaging[mArena].writable<std::uint8_t>(mLentAt, bytes);
    }

    void GuiTextures::send(const GuiSlot slot)
    {
        assert(mLentSlot == slot && "a send of a slot nothing was lent for");

        const GuiRegion region = mLentRegion;
        mLentSlot = GuiSlot::none();

        if (region.mWidth == 0 || region.mHeight == 0)
            return;

        // The two transitions are what order this against the write before it: copies into one
        // image are otherwise unordered within a submit, and a picture written twice in a frame
        // would land in whichever order the device chose.
        const Image& image = *mImages[slot.get()];
        const VkCommandBuffer commands = mBatch.getCommands();

        image.transition(commands, Use::sFragmentSample, Use::sCopyWrite);

        const VkBufferImageCopy copy{
            .bufferOffset = mLentAt,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { static_cast<std::int32_t>(region.mX), static_cast<std::int32_t>(region.mY), 0 },
            .imageExtent = { region.mWidth, region.mHeight, 1 },
        };
        vkCmdCopyBufferToImage(
            commands, mStaging[mArena].getHandle(), image.getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        image.transition(commands, Use::sCopyWrite, Use::sFragmentSample);
    }

    VkDeviceSize GuiTextures::reserve(VkDeviceSize bytes)
    {
        Buffer& arena = mStaging[mArena];
        VkDeviceSize at = (mStagingUsed + sCopyAlignment - 1) & ~(sCopyAlignment - 1);

        if (at + bytes > arena.getSize())
        {
            // Waited for before the arena is rewound or replaced, and that is the whole of the
            // safety here. What was recorded reads these bytes; handing it over would only order
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
        assert(mLentSlot.isNone() && "a hand over with a lend outstanding");

        mBatch.defer();
    }

    void GuiTextures::finish()
    {
        assert(mLentSlot.isNone() && "a finish with a lend outstanding");

        // Two calls, because the batch's own submit carries what was handed over before it only
        // when there is something left in the batch to submit.
        mBatch.flush();
        mPool.finishDeferred();

        mStagingUsed = 0;
    }

    void GuiTextures::startFrame(Graveyard& kept)
    {
        assert(mLentSlot.isNone() && "an interface frame that began with a lend outstanding");

        for (std::unique_ptr<Image>& image : mRetired)
            kept.bury(std::move(image));

        mRetired.clear();

        mArena = (mArena + 1) % sStagingArenas;
        mStagingUsed = 0;
    }

    void GuiTextures::drop(const GuiSlot slot)
    {
        assert(holds(slot) && "a slot given back twice");

        // Put aside rather than destroyed, because a copy recorded against this image may not have
        // run, and a flush here would put a round trip on every window that closes. The wait that
        // frees it is a frame's — see `startFrame`.
        mRetired.push_back(std::move(mImages[slot.get()]));
        mFree.free(slot.get());

        // The buffer stays for whatever takes the slot next; what was in it is nobody's now.
        Copy& copy = mCopies[slot.get()];
        copy.mTracedOn = Copy::sNever;
        copy.mLanded = false;
    }

    void GuiTextures::readBackWith(
        const GuiSlot slot, const VkCommandBuffer commands, const std::uint64_t frame, Graveyard& graveyard)
    {
        assert(holds(slot) && "a read back of a slot nothing holds");

        const Image& image = *mImages[slot.get()];
        const VkDeviceSize bytes = image.getReadBytes();

        Copy& copy = mCopies[slot.get()];
        if (copy.mBuffer == nullptr || copy.mBuffer->getSize() < bytes)
        {
            if (copy.mBuffer != nullptr)
                graveyard.bury(std::move(*copy.mBuffer));
            copy.mBuffer = std::make_unique<Buffer>(Buffer::staging(mDevice, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
        }

        image.recordRead(commands, Use::sFragmentSample, Use::sFragmentSample, *copy.mBuffer);

        copy.mTracedOn = frame;
        copy.mLanded = false;
    }

    bool GuiTextures::takeCopy(const GuiSlot slot, const std::span<std::uint8_t> into, const std::uint64_t finished)
    {
        assert(holds(slot) && "a copy of a slot nothing holds");

        Copy& copy = mCopies[slot.get()];
        if (copy.mTracedOn == Copy::sNever)
            return false;

        if (!copy.mLanded && copy.mTracedOn >= finished)
            return false;

        copy.mLanded = true;

        const std::size_t bytes = std::min<std::size_t>(into.size(), copy.mBuffer->getSize());
        std::memcpy(into.data(), copy.mBuffer->writable<std::uint8_t>(0, bytes).data(), bytes);
        return true;
    }

    void GuiTextures::landTraces()
    {
        for (Copy& copy : mCopies)
            if (copy.mTracedOn != Copy::sNever)
                copy.mLanded = true;
    }

    void GuiTextures::read(const GuiSlot slot, std::vector<std::uint8_t>& pixels)
    {
        assert(holds(slot) && "a read of a slot nothing holds");

        // The read back submits and waits for itself, and carries what is handed over here ahead of
        // its own copy — so the bytes it takes off the device are the ones just written.
        handOver();

        mImages[slot.get()]->read(mPool, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pixels);
    }

    VkImageView GuiTextures::getView(const GuiSlot slot)
    {
        handOver();

        if (!holds(slot))
            return VK_NULL_HANDLE;

        return mImages[slot.get()]->getView();
    }
}
