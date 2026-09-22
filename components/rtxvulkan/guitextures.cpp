#include "guitextures.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <utility>

#include <components/rtx/runs.hpp>

#include "commands.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "image.hpp"
#include "imageuse.hpp"
#include "memory.hpp"
#include "timeline.hpp"

namespace Rtx
{
    namespace
    {
        /// What `vkCmdCopyBufferToImage` requires of a buffer offset: a multiple of four and of the
        /// texel block, and every texture here is four bytes a texel.
        constexpr VkDeviceSize sCopyAlignment = 4;
    }

    GuiTextures::GuiTextures(const Device& device)
        : mDevice(device)
        , mBatch(device.getPool())
    {
    }

    GuiTextures::~GuiTextures() = default;

    GuiSlot GuiTextures::add(std::uint32_t width, std::uint32_t height)
    {
        Image image(mDevice, width, height, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            "gui texture");

        // Cleared rather than left undefined. A slot is sampleable from the moment anything can
        // observe it, so a batch drawn before the first write shows nothing instead of whatever the
        // memory held — and the pass never has to ask whether a texture is ready.
        image.clear(mBatch.getCommands(), Use::sUndefined, VkClearColorValue{}, Use::sFragmentSample);

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
        assert(region.mX + region.mWidth <= mImages[slot.get()].getWidth()
            && region.mY + region.mHeight <= mImages[slot.get()].getHeight()
            && "a region past the edge of the texture");

        const VkDeviceSize bytes = VkDeviceSize{ region.mWidth } * region.mHeight * 4;

        // Before the lend is on the books, because a run that does not fit hands what is already
        // recorded over to the next submit — and nothing may be lent across that.
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
        const Image& image = mImages[slot.get()];
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

    VkDeviceSize GuiTextures::reserve(const VkDeviceSize bytes)
    {
        Buffer& arena = mStaging[mArena];
        const VkDeviceSize at = alignUp(mStagingUsed, sCopyAlignment);

        if (at + bytes <= arena.getSize())
        {
            mStagingUsed = at + bytes;
            return at;
        }

        // **Handed over and buried, never waited for and rewound.** Once handed over, every copy
        // recorded against the arena rides the next submit, and the graveyard stamps the arena for
        // that same submit — so the bytes outlive this frame's copies and whatever the interface's
        // draw two frames back is still reading. A wait here idled the queue on every overflow,
        // and the layers still reported the arena it then destroyed as read by a pending copy.
        //
        // **Grown to the frame's writes so far and not to this one alone**, so a frame that writes
        // as much again lands in one arena: grown to the region, an arena a frame overflows with
        // two regions was replaced on every frame that wrote them.
        handOver();
        mDevice.getGraveyard().replace(arena,
            Buffer::hostWritten(
                mDevice, std::max(at + bytes, arena.getSize()), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "gui staging"));

        mStagingUsed = bytes;
        return 0;
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
        mDevice.getPool().finishDeferred();

        mStagingUsed = 0;
    }

    void GuiTextures::startFrame()
    {
        assert(mLentSlot.isNone() && "an interface frame that began with a lend outstanding");

        mArena = (mArena + 1) % sStagingArenas;
        mStagingUsed = 0;
    }

    void GuiTextures::drop(const GuiSlot slot)
    {
        assert(holds(slot) && "a slot given back twice");

        // Kept on the batch rather than buried here, because a copy recorded against this image
        // may not have run, and the batch is what knows which submit it rides; and not flushed,
        // because that would put a round trip on every window that closes. The draw two frames
        // back that sampled it is before that submit either way.
        mBatch.keep(std::move(mImages[slot.get()]));
        mFree.free(slot.get());

        // The buffer stays for whatever takes the slot next; what was in it is nobody's now.
        mCopies[slot.get()].mRides = Copy::sNever;
    }

    void GuiTextures::readBackWith(const GuiSlot slot, const VkCommandBuffer commands)
    {
        assert(holds(slot) && "a read back of a slot nothing holds");

        const Image& image = mImages[slot.get()];
        const VkDeviceSize bytes = image.getReadBytes();

        // Buried and not destroyed where it has to grow: a batch recorded against it may not have
        // run.
        Copy& copy = mCopies[slot.get()];
        growTo(copy.mBuffer, mDevice, BufferKind::ReadBack, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "gui read back");

        image.recordRead(commands, Use::sFragmentSample, Use::sFragmentSample, copy.mBuffer);

        copy.mRides = mDevice.getTimeline().getNext();
    }

    bool GuiTextures::takeCopy(const GuiSlot slot, const std::span<std::uint8_t> into)
    {
        assert(holds(slot) && "a copy of a slot nothing holds");

        const Copy& copy = mCopies[slot.get()];
        if (copy.mRides == Copy::sNever || !mDevice.getTimeline().hasFinished(copy.mRides))
            return false;

        const std::size_t bytes = std::min<std::size_t>(into.size(), copy.mBuffer.getSize());
        std::memcpy(into.data(), copy.mBuffer.map(), bytes);
        return true;
    }

    void GuiTextures::read(const GuiSlot slot, std::vector<std::uint8_t>& pixels)
    {
        assert(holds(slot) && "a read of a slot nothing holds");

        // The read back submits and waits for itself, and carries what is handed over here ahead of
        // its own copy — so the bytes it takes off the device are the ones just written.
        handOver();

        mImages[slot.get()].read(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pixels);
    }

    VkImageView GuiTextures::getView(const GuiSlot slot)
    {
        handOver();

        if (!holds(slot))
            return VK_NULL_HANDLE;

        return mImages[slot.get()].getView();
    }
}
