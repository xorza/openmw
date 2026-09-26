#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/ground.h>

#include "computepipeline.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;

    /// A chunk's layer stack flattened into one texture on the device: `groundcomposite.comp`,
    /// which says why it is here and not on threads of the host. One dispatch a chunk and the
    /// chain blitted after it, in the placement that wrote the chunk's material row, because the
    /// sum reads the scene's tables and textures as that placement bound them.
    class GroundCompositePass
    {
    public:
        /// @param textures the layout of the scene's texture set, bound at `SET_TEXTURES` — the
        ///        layers' textures and their shading maps.
        GroundCompositePass(
            const Device& device, const std::filesystem::path& shaderDirectory, VkDescriptorSetLayout textures);

        /// Records `chunk`'s bake into its albedo and its gloss, every level of each: one sum into
        /// both first levels, the chains blitted below them in step. Each is met undefined and left
        /// as a texture the trace samples, and must be `GROUND_COMPOSITE_EXTENT` square, with a
        /// storage view of a `UNORM` format and both transfer usages.
        ///
        /// @param textures the scene's texture set for the copy `chunk`'s tables are of.
        /// @param albedo,gloss null where `chunk.mOutputs` does not name it, and only there.
        void record(VkCommandBuffer commands, VkDescriptorSet textures, const Image* albedo, const Image* gloss,
            const Shaders::GroundCompositeConstants& chunk) const;

    private:
        ComputePipeline mPipeline;

        /// What an image the bake does not write is bound as, because a descriptor has to point
        /// somewhere: a chunk whose two images arrived apart is baked once for each.
        Image mNoTarget;
    };
}
