#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/micromap.h>

#include "computepipeline.hpp"

namespace Rtx
{
    class Device;

    /// Decides every microtriangle of a cutout mesh's mask on the device, ahead of the micromap
    /// build over what it wrote.
    ///
    /// **One lane per triangle**, which walks its microtriangles in curve order, decides each
    /// against the texels its footprint touches, and stores its states a word at a time — no
    /// atomics and no clear, because a lane owns its triangle's run of the data. What a state
    /// promises is `micromap.h`'s to say, and `SceneMicromaps` is what chooses each triangle's level
    /// and builds the micromap from the words.
    ///
    /// **Shared by every scene**, like `SkinPass`: it samples the scene's bindless textures and so
    /// needs the layout only a scene brings, and every array declares the same one. What differs
    /// per scene is `SceneMicromaps`.
    class MicromapPass
    {
    public:
        MicromapPass(
            const Device& device, VkDescriptorSetLayout textureLayout, const std::filesystem::path& shaderDirectory);

        MicromapPass(const MicromapPass&) = delete;
        MicromapPass& operator=(const MicromapPass&) = delete;

        /// Binds the kernel and `textures` as set one, once ahead of the dispatches that read them.
        void begin(VkCommandBuffer commands, VkDescriptorSet textures) const;

        /// One mesh's bake, into the data `constants` names. Nothing here orders the writes
        /// against the build that reads them; the caller records that barrier once for all of them.
        void bake(VkCommandBuffer commands, const Shaders::MicromapConstants& constants) const;

    private:
        ComputePipeline mPipeline;
    };
}
