#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    class GpuTimer;
    class Graveyard;

    /// Where a placement records, which copy it writes, what times it and what it may bury.
    ///
    /// **The placement's own context and not the frame's.** A pass records and is timed but writes
    /// no copy and buries nothing; a batched build buries and is timed but records through a
    /// `Batch`. Placing is the one thing that needs all four, so this is where they are named, and
    /// the rest keep their arguments rather than take fields they never read.
    struct Placing
    {
        VkCommandBuffer mCommands = VK_NULL_HANDLE;

        /// Which copy of the per-slot tables this writes. The caller has made sure no frame in
        /// flight is still reading it.
        std::uint32_t mSlot = 0;

        /// Null for a picture inside the interface, which is not timed —
        /// `VulkanRenderer::placeScene` says why.
        GpuTimer* mTimer = nullptr;

        Graveyard& mGraveyard;
    };
}
