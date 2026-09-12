#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/visibility.h>

#include "buffer.hpp"
#include "computepipeline.hpp"
#include "frameslots.hpp"
#include "tracepipeline.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class FogTile;
    class FogVolume;
    class GBuffer;
    class GpuTimer;
    class SceneBuffers;
    class WavePass;

    /// What a trace reads about the world, as against the camera that looks at it.
    struct VisibilityInputs
    {
        VkAccelerationStructureKHR mScene = VK_NULL_HANDLE;
        const SceneBuffers* mBuffers = nullptr;

        /// Which copy of the frame's tables in `mBuffers` this trace reads: the one the frame's
        /// placement wrote. The first for a scene that is traced and waited for.
        FrameSlot mSlot;

        /// Where the index blocks are, which is `SceneAcceleration`'s. Taken fresh every frame and
        /// never cached, because the table is made again whenever a block is added to it.
        VkDeviceAddress mIndexBlocks = 0;

        /// The bindless texture array's set, bound once and not pushed. Every array declares the
        /// same shape, so a set from a later array binds against the pipeline layout the first one
        /// produced.
        VkDescriptorSet mTextures = VK_NULL_HANDLE;

        /// The sea, as the tiles it was synthesised into this frame. Not the scene's, because one
        /// sea runs under every cell and under the doll and the map beside them.
        const WavePass* mWaves = nullptr;

        /// The fog's fractal field, here for the same reason and drawn once for the life of the
        /// device rather than once a frame.
        const FogTile* mFog = nullptr;

        /// Where the air in front of this camera is integrated, before the trace reads it. Sized to
        /// the camera and so not the pass's, as `GBuffer` is.
        const FogVolume* mFogVolume = nullptr;

        /// The sprite tiles' list to read in place of the slot's, or nought to read the slot's. For
        /// a camera that draws no sprites: the slot's list holds whatever the last bin into it left,
        /// sized for another camera. An empty list is two words, and one buffer serves every extent.
        VkDeviceAddress mSpriteList = 0;

        /// Whether the eye can meet water in this scene — the scene's answer and not the camera's,
        /// and what `HAS_SEA` takes the waves out of for a room.
        bool mWater = false;
    };

    /// What a trace can be told at compile time, and so what keys a pipeline. Each is only ever
    /// false where the shader's own test already answers no, so a variant takes out dead code and
    /// never an answer, and a specialized frame is the same picture byte for byte.
    /// `lib/variants.glsl` says what each removes.
    struct VisibilityVariant
    {
        bool mSun = true;
        bool mMoons = true;
        bool mSea = true;

        /// What this frame is. `water` is `VisibilityInputs::mWater`, for the reason given there.
        static VisibilityVariant resolve(const Shaders::VisibilityConstants& frame, bool water);

        /// Which of the table's pipelines this tuple is.
        std::uint32_t index() const;

        /// What a capture calls this tuple of `kernel`.
        std::string describe(std::string_view kernel) const;

        /// How many tuples there are, and so how long the table is.
        static constexpr std::uint32_t sCount = 8;
    };

    /// One ray per pixel against the top-level structure. Everything it needs arrives at record
    /// time, so nothing is allocated per frame; the frame's own description lives in a buffer this
    /// owns because it outgrew what a push constant may carry.
    class VisibilityPass
    {
    public:
        /// @param pool used once, to get the blue-noise tile onto the device. The pass owns the
        ///        tile because it belongs to the sampler and not to the scene or the camera.
        /// @param textureLayout the layout of the bindless array this will be handed at record
        ///        time, because a pipeline layout names every set it will ever see.
        /// @param channelLayout the same, for the set a `GBuffer` hands over.
        /// @param volumeLayout the same again, for the set a `FogVolume` hands over.
        /// @param countHits whether the trace counts the primary rays that hit anything — a
        ///        harness facility, specialized away rather than branched on.
        /// @param countCrossings whether it also counts the see-through surfaces each of those rays
        ///        crosses, a second traversal a pixel.
        VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
            VkDescriptorSetLayout textureLayout, const SetLayout& channelLayout, const SetLayout& volumeLayout,
            bool countHits, bool countCrossings);

        /// Records the trace, in whichever kernel this frame calls for.
        ///
        /// @param buffer where the trace leaves its channels, all four in `VK_IMAGE_LAYOUT_GENERAL`
        ///        and at least as large as the frame. It writes a picture no longer: the indirect
        ///        term has to survive to the filter with the albedo still divided out.
        /// @param hitCount a storage buffer of one `uint32` the shader increments per hit.
        /// @param historyLost whether the frame before this one is worth reprojecting into. Written
        ///        into the block as a basis of nothing, which every shader here reads as "there is
        ///        no previous frame". The fog volume's answer and not the denoisers'
        ///        (`VulkanRenderer::mAirStale`).
        /// @param timer where the three zones this records go, or nothing where nobody is counting.
        void record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
            const Buffer& hitCount, const Shaders::VisibilityConstants& constants, bool historyLost,
            GpuTimer* timer) const;

    private:
        /// Makes every kernel this pass can ever need, before it returns, because the frame path
        /// must not be able to compile: the trace took 2.8 seconds on a cold cache, and a frame
        /// that stopped for one was `Xid 109, CTX SWITCH TIMEOUT` and a device reset. In parallel,
        /// because the driver's cache is internally synchronised: twenty-four kernels take 6.3 s
        /// of wall time cold, and `PipelineCache` outlives the process.
        void compileEvery(VkDescriptorSetLayout textureLayout);

        /// The sets bound after the pushed one, in the order both kernels declare them. A pipeline
        /// layout names every set it will ever be handed, and the two kernels are handed the same.
        std::array<VkDescriptorSetLayout, 3> laterSets(VkDescriptorSetLayout textureLayout) const;

        /// Writes the frame's own block into `mConstants`, barriered against both the dispatch
        /// before it and the one after.
        void writeConstants(VkCommandBuffer commands, const Shaders::VisibilityConstants& described) const;

        /// Pushes set zero — everything both passes read — and binds the three sets nothing pushes.
        /// A layout and a bind point rather than a pipeline, because the volume reads the same
        /// world the trace does.
        void pushInputs(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
            const VisibilityInputs& inputs, const GBuffer& buffer, const Buffer& hitCount, std::uint64_t frame) const;

        /// The kernel for `variant`, which `compileEvery` made.
        const TracePipeline& pipelineFor(VisibilityVariant variant) const;

        /// The same, for the pass that fills the fog volume's froxels. Every tuple has one, a
        /// room's included: the volume walks the lamps once per froxel where the closed form would
        /// be a lamp reservoir and a shadow ray per pixel.
        const ComputePipeline& scatterPipelineFor(VisibilityVariant variant) const;

        const Device& mDevice;

        Buffer mBlueNoise;

        /// This frame's `VisibilityConstants`, on the device: a push constant until they passed
        /// 256 bytes. Written with `vkCmdUpdateBuffer`, which runs in queue order, so one buffer
        /// serves every frame.
        Buffer mConstants;

        /// Fixed for the life of the pass, where the four in `VisibilityVariant` are the frame's:
        /// what counts hits is which binary was built and not what is being looked at.
        std::uint32_t mCountHits = 0;
        std::uint32_t mCountCrossings = 0;

        /// The second of the two sets bound after the pushed one, which the renderer owns for its
        /// whole life. The first is the scene's and arrives with the frame — `mTextureLayout`.
        VkDescriptorSetLayout mChannelLayout = VK_NULL_HANDLE;

        /// The third of the sets nothing pushes, which the fog volume owns. Held for the reason
        /// `mChannelLayout` is.
        VkDescriptorSetLayout mVolumeLayout = VK_NULL_HANDLE;

        /// Where the compiled modules are, kept because a variant is compiled long after
        /// construction. The trace's are one launch's worth: the ray generation shader, the one
        /// any-hit shader every hit group names, the sky's miss shader, and one closest-hit shader
        /// per `MaterialKind` in that enum's own order, each behind a record per layer of the peel.
        std::filesystem::path mDepthModule;
        std::filesystem::path mScatterModule;
        std::filesystem::path mIntegrateModule;
        std::filesystem::path mRaygenModule;
        std::filesystem::path mAnyHitModule;
        std::array<std::filesystem::path, Shaders::MISS_RECORD_COUNT> mMissModules;
        std::array<std::filesystem::path, Shaders::HIT_SHADER_COUNT> mHitModules;

        /// One pipeline per tuple, every one of them made by `compileEvery`.
        std::array<std::unique_ptr<TracePipeline>, VisibilityVariant::sCount> mPipelines;

        /// The same table for the pass that fills the froxels.
        std::array<std::unique_ptr<ComputePipeline>, VisibilityVariant::sCount> mScatterPipelines;

        /// And one for the pass that finds where each column's ray stops, which no tuple changes:
        /// it traces and shades nothing.
        std::unique_ptr<ComputePipeline> mDepthPipeline;

        /// And one for the pass that integrates the columns, which takes no tuple at all: every
        /// question was answered by the pass that filled the froxels.
        std::unique_ptr<ComputePipeline> mIntegratePipeline;
    };
}
