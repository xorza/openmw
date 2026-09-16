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
#include "handles.hpp"
#include "pipeline.hpp"
#include "tracepipeline.hpp"

namespace Rtx
{
    class Device;
    class FogTile;
    class FogVolume;
    class GBuffer;
    class GpuTimer;
    class Image;
    class RipplePass;
    class SceneBuffers;
    class SpriteBin;
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

        /// What walked through the water, as the tiles the ripple pass unpacked it into. One field
        /// under every picture, world-anchored, so a map tile reads the same wake the frame does.
        const RipplePass* mRipples = nullptr;

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

        /// The frame as it will be shown, at the output's extent and in `GENERAL`, which
        /// `recordSpriteComposite` composites the puffs over. Bound for every launch, because one
        /// set serves them all, and read by that one alone.
        const Image* mShown = nullptr;

        /// The sun glare fader's two counts, `SunGlarePass::getCounts`, which the eye's launch
        /// adds to. Always set: a picture inside the interface adds to the frame's, harmlessly,
        /// because the frame zeroes them ahead of its own trace.
        const Buffer* mSunGlare = nullptr;

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
        VisibilityPass(const Device& device, const std::filesystem::path& shaderDirectory,
            const SetLayout& textureLayout, const SetLayout& channelLayout, const SetLayout& volumeLayout,
            bool countHits);

        /// Writes the frame's block: `constants` with what only the passes know filled in — the
        /// tiles' widths, the lamps' grid, the froxel grid and where every table is, the bin's
        /// sprites among them. Before every launch of this frame, `recordSpriteShelter` first,
        /// because that one runs before the bin and reads the block like the rest.
        ///
        /// @param bin where this trace's sprites and tiles are, which the chain recording the
        ///        trace owns and filled ahead of it.
        /// @param historyLost whether the frame before this one is worth reprojecting into. Written
        ///        into the block as a basis of nothing, which every shader here reads as "there is
        ///        no previous frame". The fog volume's answer and not the denoisers'
        ///        (`VulkanRenderer::mAirStale`).
        void writeFrame(VkCommandBuffer commands, const VisibilityInputs& inputs, const SpriteBin& bin,
            const Shaders::VisibilityConstants& constants, bool historyLost) const;

        /// Zeroes, in the bin's own table, every falling sprite that stands under a roof — one ray
        /// straight up apiece, `spriteshelter.rgen`. After `writeFrame` and the bin's `take`, and
        /// before its `record`, so the shade counts no sheltered drop as a layer and the bin lists
        /// none. Nothing for a frame the block says has no shelter in it.
        ///
        /// @param count how many sprites the bin took, which is the launch's width.
        /// @param trace which copy of the air the trace writes — `TraceRecording::mTraceSlot`.
        void recordSpriteShelter(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
            const Buffer& hitCount, const Shaders::VisibilityConstants& constants, std::uint32_t count, FrameSlot trace,
            GpuTimer* timer) const;

        /// Records the trace, in whichever kernel this frame calls for. After `writeFrame`, which
        /// is what every launch here reads.
        ///
        /// @param buffer where the trace leaves its channels, all four in `VK_IMAGE_LAYOUT_GENERAL`
        ///        and at least as large as the frame. Channels and not a picture, because the
        ///        indirect term has to survive to the filter with the albedo still divided out.
        /// @param hitCount a storage buffer of one `uint32` the shader increments per hit.
        /// @param trace which copy of the air this trace writes, the other being its history.
        /// @param timer where the three zones this records go, or nothing where nobody is counting.
        void record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
            const Buffer& hitCount, const Shaders::VisibilityConstants& constants, FrameSlot trace,
            GpuTimer* timer) const;

        /// Composites the puffs over `inputs.mShown`, in place, at the picture's own extent: the
        /// sprites' shape marched there against the bin the trace binned over its own grid, their
        /// light and the cloud shells read off the layer the trace left in `Channel::Puffs`. After
        /// whatever denoised and upscaled the frame, because neither should touch a particle —
        /// `spritecomposite.rgen` says what an upscaler's overlay costs.
        ///
        /// @param trace which copy of the air, as `record` was handed it. The block is the one
        ///        the trace wrote, so the traced camera and the bin are read from there.
        /// @param shown how much of `inputs.mShown` the picture is, from its corner: the whole of
        ///        a frame's, and a picture's own size inside an image that may be larger.
        void recordSpriteComposite(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
            const Buffer& hitCount, FrameSlot trace, VkExtent2D shown, GpuTimer* timer) const;

    private:
        /// Makes every kernel this pass can ever need, before it returns, because the frame path
        /// must not be able to compile: the trace took 2.8 seconds on a cold cache, and a frame
        /// that stopped for one was `Xid 109, CTX SWITCH TIMEOUT` and a device reset. In parallel,
        /// because the driver's cache is internally synchronised: twenty-four kernels take 6.3 s
        /// of wall time cold, and `PipelineCache` outlives the process.
        void compileEvery(const std::filesystem::path& shaders, VkDescriptorSetLayout textureLayout);

        /// The sets bound after the pushed one, in the order both kernels declare them. A pipeline
        /// layout names every set it will ever be handed, and the two kernels are handed the same.
        std::array<VkDescriptorSetLayout, 3> laterSets(VkDescriptorSetLayout textureLayout) const;

        /// Writes the frame's own block into `mConstants`, barriered against both the dispatch
        /// before it and the one after.
        void writeConstants(VkCommandBuffer commands, const Shaders::VisibilityConstants& described) const;

        /// Pushes set zero — everything both passes read — and binds the three sets nothing pushes.
        /// Any of the pipelines here, because the volume reads the same world the trace does.
        void pushInputs(VkCommandBuffer commands, const Pipeline& pipeline, const VisibilityInputs& inputs,
            const GBuffer& buffer, const Buffer& hitCount, FrameSlot trace) const;

        /// The kernel for `variant`, which `compileEvery` made.
        const TracePipeline& pipelineFor(VisibilityVariant variant) const;

        /// The same, for the launch that fills the fog volume's froxels. Every tuple has one, a
        /// room's included: the volume walks the lamps once per froxel where the closed form would
        /// be a lamp reservoir and a shadow ray per pixel.
        const TracePipeline& scatterPipelineFor(VisibilityVariant variant) const;

        const Device& mDevice;

        Buffer mBlueNoise;

        /// This frame's `VisibilityConstants`, on the device: a push constant until they passed
        /// 256 bytes. Written with `vkCmdUpdateBuffer`, which runs in queue order, so one buffer
        /// serves every frame.
        Buffer mConstants;

        /// Fixed for the life of the pass, where the four in `VisibilityVariant` are the frame's:
        /// what counts hits is which binary was built and not what is being looked at.
        std::uint32_t mCountHits = 0;

        /// The second of the two sets bound after the pushed one, which the renderer owns for its
        /// whole life. The first is the scene's and arrives with the frame — `mTextureLayout`.
        VkDescriptorSetLayout mChannelLayout = VK_NULL_HANDLE;

        /// The third of the sets nothing pushes, which the fog volume owns. Held for the reason
        /// `mChannelLayout` is.
        VkDescriptorSetLayout mVolumeLayout = VK_NULL_HANDLE;

        /// One pipeline per tuple, every one of them made by `compileEvery`.
        std::array<std::unique_ptr<TracePipeline>, VisibilityVariant::sCount> mPipelines;

        /// The same table for the launch that fills the froxels. A launch and not a dispatch, and
        /// so is the column pass under it: `fogscatter.rgen` says what a ray query answers inside a
        /// dispatch when another process shares the card.
        std::array<std::unique_ptr<TracePipeline>, VisibilityVariant::sCount> mScatterPipelines;

        /// And one for the launch that finds where each column's ray stops, which no tuple
        /// changes: it traces and shades nothing.
        std::unique_ptr<TracePipeline> mDepthPipeline;

        /// And one for the launch that composites the puffs over the shown frame, which takes no
        /// tuple either: it reads the bin and the air and traces nothing. A launch and not a
        /// dispatch for the reason `spritecomposite.rgen` gives.
        std::unique_ptr<TracePipeline> mSpriteCompositePipeline;

        /// The launch over the sprite list that keeps the rain from under the roofs. One, like the
        /// composite's: it reads the structure and the tables and has no opinion about the sky.
        std::unique_ptr<TracePipeline> mSpriteShelterPipeline;

        /// And one for the pass that integrates the columns, which takes no tuple at all: every
        /// question was answered by the pass that filled the froxels.
        std::unique_ptr<ComputePipeline> mIntegratePipeline;
    };
}
