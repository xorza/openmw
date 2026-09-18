#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/ground.h>

#include "computepipeline.hpp"

namespace Rtx
{
    class Device;
    class Image;

    /// A chunk's layer stack flattened into one texture on the device: `groundcomposite.comp`,
    /// which says why it is here and not on threads of the host. One dispatch a chunk and the
    /// chain blitted after it, in the placement that wrote the chunk's material row, because the
    /// sum reads the scene's tables and textures as that placement bound them.
    class GroundCompositePass
    {
    public:
        /// @param textures the layout of the scene's texture set, bound as set one — the layers'
        ///        textures and their shading maps.
        GroundCompositePass(
            const Device& device, const std::filesystem::path& shaderDirectory, VkDescriptorSetLayout textures);

        /// Records `chunk`'s bake into `composite`, every level: the sum into the first, the chain
        /// blitted below it. `composite` is met undefined and left as a texture the trace samples;
        /// it must be `GROUND_COMPOSITE_EXTENT` square, with a storage view of a `UNORM` format and
        /// both transfer usages.
        ///
        /// @param textures the scene's texture set for the copy `chunk`'s tables are of.
        void record(VkCommandBuffer commands, VkDescriptorSet textures, const Image& composite,
            const Shaders::GroundCompositeConstants& chunk) const;

    private:
        ComputePipeline mPipeline;
    };
}
