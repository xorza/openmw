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

namespace Rtx
{
    class Batch;
    class Device;
    class FogTile;
    class FogVolume;
    class FogVolumeLayout;
    class GBuffer;
    class GBufferLayout;
    class GpuTimer;
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
        ///
        /// **The set and not the layout it came from.** Every array declares the same shape, so a
        /// set from a later array binds against the pipeline layout the first one produced — and
        /// `compileEvery` makes every kernel before any frame runs, so no layout handle has to
        /// survive a scene rebuild to reach a pipeline being built.
        VkDescriptorSet mTextures = VK_NULL_HANDLE;

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

        /// Where the air in front of this camera is integrated, before the trace reads it.
        ///
        /// **Sized to the camera and so not the pass's**, which is the same reason `GBuffer` arrives
        /// here rather than being held: a frame, a doll and a map tile are three sizes, and the pass
        /// outlives all of them.
        const FogVolume* mFogVolume = nullptr;

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

        /// What a capture calls this tuple of `kernel`.
        std::string describe(std::string_view kernel) const;

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
        /// @param volumeLayout the same again, for the set a `FogVolume` hands over.
        /// @param countHits whether the trace counts the primary rays that hit anything. A harness
        ///        facility: `shot` prints it and a test asserts on it, and nothing in the game reads
        ///        it — so it is specialized away rather than branched on, and the game's module
        ///        carries no atomic at all.
        VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
            VkDescriptorSetLayout textureLayout, const GBufferLayout& channelLayout,
            const FogVolumeLayout& volumeLayout, bool countHits);

        VisibilityPass(const VisibilityPass&) = delete;
        VisibilityPass& operator=(const VisibilityPass&) = delete;

        /// Records the trace, in whichever kernel this frame calls for.
        ///
        /// @param buffer where the trace leaves its channels, all four in `VK_IMAGE_LAYOUT_GENERAL`
        ///        and at least as large as the frame. It writes a picture no longer: the indirect
        ///        term has to survive to the filter with the albedo still divided out.
        /// @param hitCount a storage buffer of one `uint32` the shader increments per hit.
        /// @param timer where the two zones this records go, or nothing where nobody is counting.
        ///        The fog volume and the trace are two dispatches and one of them is new, so they
        ///        are timed apart — and the pass opens them because it is what decides whether the
        ///        first happens at all.
        void record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
            const Buffer& hitCount, const Shaders::VisibilityConstants& constants, GpuTimer* timer) const;

    private:
        /// Makes every kernel this pass can ever need, before it returns.
        ///
        /// **The frame path must not be able to compile, and this is what makes that true.** The
        /// trace took 2.8 seconds on a cold cache, and a frame that stopped for one held its
        /// swapchain image and its submitted work for the whole of it: the driver answered with
        /// `Xid 109, CTX SWITCH TIMEOUT` and reset the device. Two of the four constants turn with
        /// the hour, so walking the clock walked straight into it.
        ///
        /// **In parallel, because the driver's cache is internally synchronised** and the tuples are
        /// independent. Twenty-four kernels on a cold cache take 6.3 s of wall time against about
        /// a minute of compiler, and `PipelineCache` outlives the process — so a warm run pays
        /// nothing and the cold one is a load screen rather than a frame.
        void compileEvery(VkDescriptorSetLayout textureLayout);

        /// The sets bound after the pushed one, in the order both kernels declare them. A pipeline
        /// layout names every set it will ever be handed, and the two kernels are handed the same.
        std::array<VkDescriptorSetLayout, 3> laterSets(VkDescriptorSetLayout textureLayout) const;

        /// Writes the frame's own block into `mConstants`, barriered against both the dispatch
        /// before it and the one after.
        void writeConstants(VkCommandBuffer commands, const Shaders::VisibilityConstants& described) const;

        /// Pushes set zero — everything both dispatches read — and binds the three sets nothing
        /// pushes.
        ///
        /// **Both of them, because the two dispatches read the same world.** A volume asks the same
        /// questions of the same tables the trace does: it traces shadow rays against the same
        /// structure, resolves the same alpha out of the same textures, and reads the same lamps.
        void pushInputs(VkCommandBuffer commands, const ComputePipeline& pipeline, const VisibilityInputs& inputs,
            const GBuffer& buffer, const Buffer& hitCount) const;

        /// The kernel for `variant`, which `compileEvery` made.
        const ComputePipeline& pipelineFor(VisibilityVariant variant) const;

        /// The same, for the pass that fills the fog volume. Null for an even air, which reads the
        /// closed form and dispatches no volume.
        const ComputePipeline* volumePipelineFor(VisibilityVariant variant) const;

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

        /// The third of the sets nothing pushes, which the fog volume owns. Held for the reason
        /// `mChannelLayout` is.
        VkDescriptorSetLayout mVolumeLayout = VK_NULL_HANDLE;

        /// Where the compiled modules are, kept because a variant is compiled long after
        /// construction.
        std::filesystem::path mModule;
        std::filesystem::path mVolumeModule;

        /// One pipeline per tuple, every one of them made by `compileEvery`.
        std::array<std::unique_ptr<ComputePipeline>, VisibilityVariant::sCount> mPipelines;

        /// The same table for the volume, of which only the half with `mUniformFog` false is
        /// filled: a room reads the closed form and no volume is dispatched for it.
        std::array<std::unique_ptr<ComputePipeline>, VisibilityVariant::sCount> mVolumePipelines;
    };
}
