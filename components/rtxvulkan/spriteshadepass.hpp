#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/spriteshade.h>

#include "computepipeline.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;

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

        SpriteShadePass(const SpriteShadePass&) = delete;
        SpriteShadePass& operator=(const SpriteShadePass&) = delete;

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
