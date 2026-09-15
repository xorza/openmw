#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "frameslots.hpp"

namespace Rtx
{
    class GpuTimer;

    /// Where a placement records, which copy it writes and what times it — the placement's own
    /// context and not the frame's, handed to both halves of one placement so the two cannot be
    /// told two things. What a placement buries goes to the graveyard each half holds.
    struct Placing
    {
        VkCommandBuffer mCommands = VK_NULL_HANDLE;

        /// Which copy of the per-slot tables this writes. The caller has made sure no frame in
        /// flight is still reading it.
        FrameSlot mSlot;

        /// Null for a picture inside the interface, which is not timed —
        /// `VulkanRenderer::placeScene` says why.
        GpuTimer* mTimer = nullptr;
    };
}
