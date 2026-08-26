#pragma once

#include <cstdint>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/visibility.h>

#include "buffer.hpp"
#include "computepipeline.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class GBuffer;
    class SceneBuffers;

    /// What a trace reads about the world, as against the camera that looks at it.
    struct VisibilityInputs
    {
        VkAccelerationStructureKHR mScene = VK_NULL_HANDLE;
        const SceneBuffers* mBuffers = nullptr;

        /// Where the index blocks are, which is `SceneAcceleration`'s: the build had to have the
        /// indices first, and a shader needs the same ones at a hit.
        ///
        /// **Taken fresh every frame and never cached**, because the table is made again whenever a
        /// block is added to it. A handle copied once is a handle to a buffer a later arrival
        /// destroyed, and what that costs is the device.
        VkBuffer mIndexBlocks = VK_NULL_HANDLE;

        /// The bindless texture array's set, bound once and not pushed.
        VkDescriptorSet mTextures = VK_NULL_HANDLE;

        /// Every texture's shading map, which the array owns beside the textures themselves.
        VkBuffer mShading = VK_NULL_HANDLE;

        /// Where the scene's lamps were binned.
        VkBuffer mGrid = VK_NULL_HANDLE;
    };

    /// One ray per pixel against the top-level structure, shaded by the geometric normal it hit.
    ///
    /// Everything it needs arrives at record time — no descriptor pool, no set to allocate, and so
    /// nothing for it to allocate per frame either. The frame's own description is the exception and
    /// only just: it lives in a buffer this owns because it outgrew what a push constant may carry,
    /// and that buffer is made once and rewritten in place.
    class VisibilityPass
    {
    public:
        /// @param pool used once, to get the blue-noise tile onto the device. The pass owns the
        ///        tile because it belongs to the sampler and not to the scene or the camera: it is
        ///        the same numbers whatever is being looked at.
        /// @param textureLayout the layout of the bindless array this will be handed at record
        ///        time. Needed here because a pipeline layout names every set it will ever see.
        /// @param countHits whether the trace counts the primary rays that hit anything. A harness
        ///        facility: `shot` prints it and a test asserts on it, and nothing in the game reads
        ///        it — so it is specialized away rather than branched on, and the game's module
        ///        carries no atomic at all.
        VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
            VkDescriptorSetLayout textureLayout, bool countHits);

        VisibilityPass(const VisibilityPass&) = delete;
        VisibilityPass& operator=(const VisibilityPass&) = delete;

        /// @param buffer where the trace leaves its channels, all four in `VK_IMAGE_LAYOUT_GENERAL`
        ///        and at least as large as the frame. It writes a picture no longer: the indirect
        ///        term has to survive to the filter with the albedo still divided out.
        /// @param hitCount a storage buffer of one `uint32` the shader increments per hit.
        void record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
            const Buffer& hitCount, const Shaders::VisibilityConstants& constants) const;

    private:
        Buffer mBlueNoise;

        /// This frame's `VisibilityConstants`, on the device.
        ///
        /// **They were a push constant until they passed 256 bytes**, which is the whole of
        /// `maxPushConstantsSize` on the hardware this targets. Written with `vkCmdUpdateBuffer`
        /// rather than through a mapping: the write is recorded into the command buffer and so runs
        /// in queue order, which is what lets one buffer serve every frame without a second copy to
        /// keep the host and the device apart.
        Buffer mConstants;

        /// **Declared before the pipeline it specializes**, because the pipeline reads it during its
        /// own construction and members are built in declaration order.
        std::uint32_t mCountHits = 0;

        ComputePipeline mPipeline;
    };
}
