#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/guirenderer.hpp>

#include "buffer.hpp"
#include "frameslots.hpp"
#include "guipass.hpp"
#include "guitextures.hpp"

namespace Rtx
{
    class Device;
    class Image;

    /// The interface over the frame: its pass, the textures it samples, and a ring of its own beside
    /// the frame's. Drawn after the frame is submitted and waited for by nobody but the draw two
    /// behind it, because a menu is drawn on frames with no world and the frame has nothing to gain
    /// by being held open for it.
    class GuiDrawer
    {
    public:
        /// @param targetFormat what the frame's targets are, which the pass draws into.
        GuiDrawer(const Device& device, const std::filesystem::path& shaders, VkFormat targetFormat);

        GuiTextures& getTextures() { return mTextures; }
        const GuiTextures& getTextures() const { return mTextures; }

        /// Draws `batches` of `vertices` over `target`, which rests in `Use::sComputeWrite` and is
        /// left in `Use::sAnyGeneralRead`, where the presenter blits and a read back copies. Its
        /// own submit, and not waited for.
        void draw(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches, const Image& target);

    private:
        /// What one draw records into and draws out of. The vertices carry the submit that bound
        /// them, which is what says the slot is free again.
        struct Slot
        {
            VkCommandBuffer mCommands = VK_NULL_HANDLE;

            /// Rewritten every draw and grown to the busiest one so far. Host-visible device memory,
            /// so writing it is a memcpy and there is no staging copy and no transfer to record.
            Buffer mVertices;
        };

        const Device& mDevice;
        GuiPass mPass;
        GuiTextures mTextures;

        /// The batches, resolved from slots to what the pass wants. Kept so that a draw allocates
        /// nothing.
        std::vector<GuiDraw> mDraws;

        PerSlot<Slot> mSlots;

        /// How many draws there were, which picks the slot: the interface runs on its own count.
        std::uint64_t mDrawn = 0;
    };
}
