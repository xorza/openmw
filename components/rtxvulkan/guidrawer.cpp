#include "guidrawer.hpp"

#include <cassert>
#include <vector>

#include "commands.hpp"
#include "device.hpp"
#include "image.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    GuiDrawer::GuiDrawer(const Device& device, const std::filesystem::path& shaders, const VkFormat targetFormat)
        : mDevice(device)
        , mPass(device, shaders, targetFormat)
        , mTextures(device)
    {
        // Allocated once and recorded into again.
        const std::vector<VkCommandBuffer> commands = mDevice.getPool().allocate(sFrameSlots);
        for (std::uint32_t slot = 0; slot < sFrameSlots; ++slot)
            mSlots.at(FrameSlot{ slot }).mCommands = commands[slot];
    }

    void GuiDrawer::draw(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches, const Image& target)
    {
        // The interface drawn two draws ago drew out of this slot, and the vertices carry the
        // submit that bound them: that passed is what says they may be written over.
        Slot& slot = mSlots.at(FrameSlot{ static_cast<std::uint32_t>(mDrawn % sFrameSlots) });
        if (!slot.mVertices.isIdle())
            slot.mVertices.waitIdle("the interface drawn two frames ago");

        // After the wait, which collected, and before anything is handed over: this draw's submit
        // is the first that says every draw with a texture given back has finished, and the staging
        // turns on the same signal.
        mTextures.startFrame();

        outgrow(slot.mVertices, mDevice, BufferKind::HostWritten, vertices.size_bytes(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "gui vertices");
        slot.mVertices.write(vertices);

        // Named by hand, because a vertex buffer is bound by handle and not handed out as an
        // address or a descriptor.
        slot.mVertices.nameForNext();

        mDraws.clear();
        mDraws.reserve(batches.size());
        for (const GuiBatch& batch : batches)
        {
            const VkImageView view = mTextures.getView(batch.mTexture);
            assert(view != VK_NULL_HANDLE && "a batch names a texture this renderer does not hold");

            // A slot nothing holds would be a null descriptor, which is undefined rather than
            // blank. The assert above is where a caller finds out; a release build drops the batch.
            if (view != VK_NULL_HANDLE)
                mDraws.push_back(GuiDraw{ view, batch.mFirstVertex, batch.mVertexCount,
                    batch.mBlend == GuiBlend::Additive ? Blend::Additive : Blend::Over });
        }

        const VkCommandBuffer commands = slot.mCommands;
        mDevice.getPool().begin(commands);
        target.transition(commands, Use::sComputeWrite, Use::sColourAttachment);
        mPass.record(commands, target, slot.mVertices.getHandle(), mDraws);
        target.transition(commands, Use::sColourAttachment, Use::sAnyGeneralRead);
        mDevice.getPool().submit(commands);

        ++mDrawn;
    }
}
