#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include "computepipeline.hpp"

namespace Rtx
{
    class Device;
    class Image;

    /// The chain a file did not carry, made on the device as the texture arrives: `mipchain.comp`,
    /// which says why it is here and not on the host. One dispatch a level, the first fetching the
    /// upload and every other boxing the level above it.
    class MipChainPass
    {
    public:
        MipChainPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Records `chain`'s levels from `source`: the first is `source` texel for texel and the
        /// rest each the box over the one above. `source` is one level, met as a texture the trace
        /// samples, which is how an upload leaves it, and in a format with no curve under it, so
        /// a fetch is the stored bytes; `chain` is met undefined and left as a texture the trace
        /// samples, with a storage view a level in a format with no curve under it either.
        ///
        /// @param sampler any sampler the array binds: the dispatch fetches by texel and reads
        ///        none of it, but a combined image needs one.
        /// @param encoded whether `chain`'s own format is display-encoded, so the box averages in
        ///        light and writes back encoded.
        void record(
            VkCommandBuffer commands, const Image& source, VkSampler sampler, const Image& chain, bool encoded) const;

    private:
        ComputePipeline mPipeline;
    };
}
