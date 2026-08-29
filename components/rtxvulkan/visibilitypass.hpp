#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/visibility.h>

#include "buffer.hpp"
#include "computepipeline.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class FogTile;
    class GBuffer;
    class GBufferLayout;
    class SceneBuffers;
    class WavePass;

    /// What a trace reads about the world, as against the camera that looks at it.
    struct VisibilityInputs
    {
        VkAccelerationStructureKHR mScene = VK_NULL_HANDLE;
        const SceneBuffers* mBuffers = nullptr;

        /// Which copy of the frame's tables in `mBuffers` this trace reads: the one the frame's
        /// placement wrote. Nought for a scene that is traced and waited for.
        std::uint32_t mSlot = 0;

        /// Where the index blocks are, which is `SceneAcceleration`'s: the build had to have the
        /// indices first, and a shader needs the same ones at a hit.
        ///
        /// **Taken fresh every frame and never cached**, because the table is made again whenever a
        /// block is added to it. A handle copied once is a handle to a buffer a later arrival
        /// destroyed, and what that costs is the device.
        VkBuffer mIndexBlocks = VK_NULL_HANDLE;

        /// The bindless texture array's set, bound once and not pushed.
        VkDescriptorSet mTextures = VK_NULL_HANDLE;

        /// The layout that set was allocated with.
        ///
        /// **Taken fresh every frame for the reason `mIndexBlocks` is.** A scene makes its own
        /// `TextureArray` and the array makes its own layout, so a rebuild leaves the one this pass
        /// was constructed with destroyed — and a variant compiled later needs a live handle, since
        /// a pipeline layout names every set it will ever be handed. A pipeline already made carries
        /// its own copy of the definition and is not affected.
        VkDescriptorSetLayout mTextureLayout = VK_NULL_HANDLE;

        /// Every texture's shading map, which the array owns beside the textures themselves.
        VkBuffer mShading = VK_NULL_HANDLE;

        /// The sea, as the tiles it was synthesised into this frame.
        ///
        /// **Not the scene's, because the water is not.** One sea runs under every cell and under
        /// the doll and the map beside them, so it belongs to the renderer and arrives here rather
        /// than through a `SceneBuffers` that would hold one copy of it per scene.
        const WavePass* mWaves = nullptr;

        /// The fog's fractal field, here for the same reason and drawn once for the life of the
        /// device rather than once a frame.
        const FogTile* mFog = nullptr;

        /// Whether the eye can meet water in this scene.
        ///
        /// **The scene's answer and not the camera's**, which is why it is here: the frame's own
        /// block says where a surface would be and never whether there is one, and a room with
        /// neither is what `HAS_SEA` takes the waves out of.
        bool mWater = false;
    };

    /// What a trace can be told at compile time, and so what keys a pipeline.
    ///
    /// **Each of these is only ever false where the shader's own test already answers no**, so a
    /// variant takes out dead code and never an answer — which is what makes a specialized frame the
    /// same picture, byte for byte, as the one kernel drew. `lib/variants.glsl` is the other half of
    /// it, and says what each removes.
    struct VisibilityVariant
    {
        bool mSun = true;
        bool mMoons = true;
        bool mSea = true;
        bool mUniformFog = false;

        /// What this frame is. `water` is `VisibilityInputs::mWater`, for the reason given there.
        static VisibilityVariant resolve(const Shaders::VisibilityConstants& frame, bool water);

        /// Which of the table's pipelines this tuple is.
        std::uint32_t index() const;

        /// What a capture calls it.
        std::string describe() const;

        /// How many tuples there are, and so how long the table is.
        static constexpr std::uint32_t sCount = 16;
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
        /// @param channelLayout the same, for the set a `GBuffer` hands over — and the reason it
        ///        outlives any one of them, since this is created once and they are not.
        /// @param countHits whether the trace counts the primary rays that hit anything. A harness
        ///        facility: `shot` prints it and a test asserts on it, and nothing in the game reads
        ///        it — so it is specialized away rather than branched on, and the game's module
        ///        carries no atomic at all.
        VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
            VkDescriptorSetLayout textureLayout, const GBufferLayout& channelLayout, bool countHits);

        VisibilityPass(const VisibilityPass&) = delete;
        VisibilityPass& operator=(const VisibilityPass&) = delete;

        /// Records the trace, in whichever kernel this frame calls for.
        ///
        /// Not `const`, because the tuple it resolves to may be one nothing has compiled yet.
        ///
        /// @param buffer where the trace leaves its channels, all four in `VK_IMAGE_LAYOUT_GENERAL`
        ///        and at least as large as the frame. It writes a picture no longer: the indirect
        ///        term has to survive to the filter with the albedo still divided out.
        /// @param hitCount a storage buffer of one `uint32` the shader increments per hit.
        void record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
            const Buffer& hitCount, const Shaders::VisibilityConstants& constants);

    private:
        /// The pipeline for `variant`, compiled on the first frame that asks for one.
        ComputePipeline& pipelineFor(VisibilityVariant variant, VkDescriptorSetLayout textureLayout);

        const Device& mDevice;

        Buffer mBlueNoise;

        /// This frame's `VisibilityConstants`, on the device.
        ///
        /// **They were a push constant until they passed 256 bytes**, which is the whole of
        /// `maxPushConstantsSize` on the hardware this targets. Written with `vkCmdUpdateBuffer`
        /// rather than through a mapping: the write is recorded into the command buffer and so runs
        /// in queue order, which is what lets one buffer serve every frame without a second copy to
        /// keep the host and the device apart.
        Buffer mConstants;

        /// Fixed for the life of the pass, where the four in `VisibilityVariant` are the frame's:
        /// what counts hits is which binary was built and not what is being looked at.
        std::uint32_t mCountHits = 0;

        /// The second of the two sets bound after the pushed one, which the renderer owns for its
        /// whole life. The first is the scene's and arrives with the frame — `mTextureLayout`.
        VkDescriptorSetLayout mChannelLayout = VK_NULL_HANDLE;

        /// Where the compiled module is, kept because a variant is compiled long after construction.
        std::filesystem::path mModule;

        /// One pipeline per tuple, and most of them never made.
        ///
        /// **Compiled on the frame that first asks, and three of them ahead of any frame at all.**
        /// This kernel takes about half a second to compile on a cold pipeline cache, and a frame
        /// that stops for one is a stop the player sees — so the exterior day, the exterior night
        /// and the interior are made at construction, which is a load. The rest are the odd hours
        /// and the odd cells, and they cost their hitch once per build: `PipelineCache` outlives the
        /// process and every run after the first finds them.
        std::array<std::unique_ptr<ComputePipeline>, VisibilityVariant::sCount> mPipelines;
    };
}
