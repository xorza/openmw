#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include "computepipeline.hpp"

namespace Rtx
{
    class Device;
    class Image;

    /// A sprite texture's light bake, made on the device from the texture's alpha as the bake's
    /// slot arrives: `spritelight.comp`, which says what the bake is and why it is here and not on
    /// the host. One dispatch a level, into a chain shaped like the source's.
    class SpriteLightPass
    {
    public:
        SpriteLightPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Records `source`'s bake into `bake`, every level. `source` is met as a texture the trace
        /// samples, which is how an upload leaves it; `bake` is met undefined and left the same way,
        /// ready for the array's sampler. `bake` must hold as many levels as `source` and be
        /// writable as storage at each.
        ///
        /// @param sampler any sampler the array binds: the dispatch fetches by texel and reads
        ///        none of it, but a combined image needs one.
        void record(VkCommandBuffer commands, const Image& source, VkSampler sampler, const Image& bake) const;

    private:
        ComputePipeline mPipeline;
    };
}
