#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/spritebin.h>

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

        SpriteBinPass(const SpriteBinPass&) = delete;
        SpriteBinPass& operator=(const SpriteBinPass&) = delete;

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
}
