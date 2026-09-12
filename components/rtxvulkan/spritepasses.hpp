#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/spritebin.h>
#include <components/rtx/shaders/spriteshade.h>

#include "computepipeline.hpp"

namespace Rtx
{
    class Buffer;
    class Device;
    class GpuTimer;

    /// Bins the sprite layer into the screen's tiles, on the device, ahead of the trace that reads
    /// the tiles. `shaders/spritebin.h` says why this moved off the host; nothing here depends on
    /// which scene it bins, so the world and a picture inside the interface both use it.
    class SpriteBinPass
    {
    public:
        SpriteBinPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Records the bin into `commands`, into a frame's own copy of the list that the frame
        /// before last finished with. What is recorded after this reads the list through the
        /// barrier this ends with, and the host reads the report `bin` names after the fence.
        ///
        /// @param list the buffer `bin.mList` addresses: the fill that zeroes the head needs its
        ///        handle, and the assert that the list is as long as `bin` says needs its size.
        void record(VkCommandBuffer commands, const Shaders::SpriteBinConstants& bin, const Buffer& list,
            GpuTimer* timer) const;

    private:
        ComputePipeline mRects;
        ComputePipeline mStarts;
        ComputePipeline mRuns;
    };

    /// Counts how many layers of its own emitter stand between each sprite and each light, on the
    /// device, ahead of the bin and the trace that read them. `shaders/spriteshade.h` says why
    /// nothing else computes it. Shared between scenes as `SpriteBinPass` is.
    class SpriteShadePass
    {
    public:
        SpriteShadePass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Records the shading into `commands`, writing each sprite's two layer counts in place in
        /// a frame's own copy. What is recorded after this reads them through the barrier this
        /// ends with.
        void record(VkCommandBuffer commands, const Shaders::SpriteShadeConstants& shade, GpuTimer* timer) const;

    private:
        ComputePipeline mShade;
    };
}
