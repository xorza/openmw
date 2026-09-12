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
    /// the tiles.
    ///
    /// **What the host did every frame, done where the sprites already are.** `shaders/spritebin.h`
    /// says what moved and why; this is the three dispatches and the barriers between them, and
    /// nothing about it depends on which scene it bins — the world's and a picture's inside the
    /// interface both hand it their own tables. `SkinPass` is shared the same way.
    class SpriteBinPass
    {
    public:
        SpriteBinPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Records the bin into `commands`: the list's head zeroed, every sprite's tiles counted,
        /// the counts turned into starts, and every tile's run filled in order. What is recorded
        /// after this reads the list through the barrier this ends with.
        ///
        /// **Into tables nothing is reading**, which the caller guarantees: a frame's own copy of
        /// the list, which the frame before last has finished with. The report `bin` names is
        /// written for the host to read after this submit's fence.
        ///
        /// @param list the buffer `bin.mList` addresses. The fill that zeroes the head needs its
        ///        handle, and the assert that the list is as long as `bin` says needs its size —
        ///        which is why this takes the buffer and not the handle alone.
        void record(VkCommandBuffer commands, const Shaders::SpriteBinConstants& bin, const Buffer& list,
            GpuTimer* timer) const;

    private:
        ComputePipeline mRects;
        ComputePipeline mStarts;
        ComputePipeline mRuns;
    };

    /// Counts how many layers of its own emitter stand between each sprite and each light, on the
    /// device, ahead of the bin and the trace that read them.
    ///
    /// **Done where the sprites already are.** `shaders/spriteshade.h` says what this computes and
    /// why nothing else computes it. This is one dispatch and the barrier around it, and nothing
    /// about it depends on
    /// which scene it shades — the world's and a picture's inside the interface both hand it their
    /// own tables, as they do to `SpriteBinPass`.
    class SpriteShadePass
    {
    public:
        SpriteShadePass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Records the shading into `commands`, writing each sprite's two layer counts in place.
        /// What is recorded after this reads them through the barrier this ends with.
        ///
        /// **Into a table nothing is reading**, which the caller guarantees the same way the bin's
        /// caller does: a frame's own copy, which the frame before last has finished with.
        void record(VkCommandBuffer commands, const Shaders::SpriteShadeConstants& shade, GpuTimer* timer) const;

    private:
        ComputePipeline mShade;
    };
}
