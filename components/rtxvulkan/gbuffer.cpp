#include "gbuffer.hpp"
#include <algorithm>
#include <array>
#include <cassert>

#include <components/rtx/shaders/gbuffer.h>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// Full floats for the three radiance channels, and half floats were tried first.
        ///
        /// **Eleven bits of mantissa is an eighth of a display byte**, which is the argument for
        /// halving fifty megabytes, and it is wrong in the one place that matters. A reference is a
        /// sum of a thousand frames, and rounding every term before adding it is exactly what the
        /// float accumulator exists to avoid — the error only averages away if it is random, and
        /// here it is not. Two reasons: `direct` is all but identical from frame to frame, so its
        /// rounding is a fixed offset that never averages at all; and the sampler is a
        /// low-discrepancy sequence rather than a random one, so even the terms that do vary vary
        /// in a pattern the rounding follows.
        ///
        /// Measured: the converged mean of a flat surface came out 0.096% low, against a tolerance
        /// of 0.067% that the test derives from what the format can show. Full floats put it back.
        constexpr VkFormat sRadiance = GBUFFER_RADIANCE;

        /// Half floats, and `gbuffer.h` has the angles the width is derived from.
        ///
        /// **A normal quantised is not a normal summed**, which is the whole of why this moved and
        /// the radiance channels did not: the error a half float puts on a direction is compared
        /// against a neighbour's and then thrown away, where the error it puts on a radiance is
        /// added to a thousand of its own kind.
        constexpr VkFormat sGuide = GBUFFER_GUIDE;

        /// **Half floats, because an albedo is a fraction and is never accumulated.** The argument
        /// above is about summing a thousand frames into a reference; a specular albedo is a guide
        /// an upscaler divides by once and never adds to, so eleven bits of mantissa across zero to
        /// one is more resolution than the quantity has meaning at.
        ///
        /// **The diffuse albedo takes it too, and that needed measuring rather than arguing.** The
        /// case against is the one above: an albedo is a per-pixel constant, so quantising it is a
        /// systematic error on every frame's indirect term and systematic error is exactly what an
        /// average does not remove. The case for is that it multiplies only the bounce, which is a
        /// small share of a frame.
        ///
        /// Measured on a sixty-four sample reference of the mages guild, where the indirect share is
        /// as high as this renderer gets indoors: the converged mean moved by 0.0014%, against the
        /// 0.067% the radiance channels were put back to full floats over. Fifty times inside it.
        constexpr VkFormat sAlbedo = GBUFFER_ALBEDO;

        /// Two full floats, for the reason `getMotion` gives.
        constexpr VkFormat sMotion = GBUFFER_MOTION;

        /// Two, and full floats rather than halves: a clip depth puts most of its precision within a
        /// few units of the eye, so what is left at the far end of a Morrowind view is exactly where
        /// a coarse format would run out, and the distance beside it runs past thirty thousand units
        /// where a half's steps are thirty-two units wide.
        ///
        /// **Two channels where the upscaler's guide asks for one, and it costs nothing.** NGX reads
        /// the first and is handed the pair; splitting them would save no memory — two `R32_SFLOAT`
        /// images are the same eight bytes a texel as one `R32G32_SFLOAT` — so the only question was
        /// what NGX pays to sample the wider one. Timed at 1920x1080 into performance, forty frames,
        /// the upscale zone measured 1.223, 1.228 and 1.223 ms against a single-channel depth and
        /// 1.227, 1.221 and 1.224 ms against this one. The ranges overlap, so the packing stays and
        /// the two answers stay together.
        constexpr VkFormat sDepth = GBUFFER_DEPTH;

        /// Half floats for the layer the eye sees through, where the radiance channels take full
        /// ones.
        ///
        /// **The argument above does not reach it.** Full floats are there so a reference summed
        /// over a thousand frames does not carry a systematic rounding, and nothing sums this: it is
        /// read once by Ray Reconstruction and by nothing else, and a reference is built with the
        /// upscaler off — `main.cpp` sets `Upscale::Off` for `--accumulate` and `verify.cpp` for
        /// every view it renders. So the one path that would care never reads the channel at all.
        ///
        /// Eight bytes a pixel rather than sixteen, on a channel handed to the upscaler every frame.
        constexpr VkFormat sLayer = GBUFFER_LAYER;

        /// Three channels for one number, and a byte apiece.
        ///
        /// **Three channels for a reason that is not precision.** A single-channel image handed to
        /// `pInTransparencyLayerOpacity` is read as a colour rather than as a scalar, so a coverage
        /// in red alone dimmed the frame's red and left its green and blue whole — grey smoke over a
        /// blue sky came out cyan. `lib/bindings.glsl` says the rest.
        ///
        /// **A byte apiece because a coverage is a fraction**, which is the same argument the two
        /// masks beside it make and the reason they are bytes. Four bytes a pixel rather than eight,
        /// on a channel handed to the upscaler every frame.
        constexpr VkFormat sLayerOpacity = GBUFFER_LAYER_OPACITY;

        /// One byte for a yes or a no, and for a value between nought and one. `gbuffer.h` argues
        /// why a byte is enough for both and what a float cost.
        constexpr VkFormat sMask = GBUFFER_MASK;

        /// Three bytes for three fractions, which is what `gbuffer.h` argues a modulation is.
        constexpr VkFormat sStars = GBUFFER_STARS;

        /// **`SAMPLED` on all of them, and it is not decoration.** DLSS samples every input it is
        /// handed; one without the bit reads as zero, NGX returns success and the validation layers
        /// say nothing, so the whole frame comes back black with nothing pointing at the cause. It
        /// costs no memory, so every channel carries it rather than only the five DLSS reads today.
        constexpr VkImageUsageFlags sUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        /// The channels a caller can ask to read back: the bounce, the two motion fields, the depth
        /// and the two masks. See `Rtx::Channel`.
        constexpr VkImageUsageFlags sReadable = sUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    GBuffer::GBuffer(const Device& device, const SetLayout& layout, std::uint32_t width, std::uint32_t height)
        : mDevice(device)
        , mDirect(device, width, height, sRadiance, sUsage, "g-direct")
        , mIndirect(device, width, height, sRadiance, sReadable, "g-indirect")
        , mAlbedo(device, width, height, sAlbedo, sUsage, "g-albedo")
        , mSpecular(device, width, height, sAlbedo, sUsage, "g-specular")
        , mGuide(device, width, height, sGuide, sUsage, "g-guide")
        , mMotion(device, width, height, sMotion, sReadable, "g-motion")
        , mDepth(device, width, height, sDepth, sReadable, "g-depth")
        , mReflectionMotion(device, width, height, sMotion, sReadable, "g-reflection-motion")
        , mParticleMask(device, width, height, sMask, sReadable, "g-particle-mask")
        , mBiasMask(device, width, height, sMask, sReadable, "g-bias-mask")
        , mStarsShown(device, width, height, sStars, sUsage, "g-stars-shown")
        , mTransparency(device, width, height, sLayer, sUsage, "g-transparency")
        , mTransparencyOpacity(device, width, height, sLayerOpacity, sUsage, "g-transparency-opacity")
        , mTransparencyMotion(device, width, height, sMotion, sUsage, "g-transparency-motion")
    {
        try
        {
            const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sChannels };
            const VkDescriptorPoolCreateInfo describePool{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 1,
                .poolSizeCount = 1,
                .pPoolSizes = &size,
            };
            checkVk(
                vkCreateDescriptorPool(mDevice.getHandle(), &describePool, nullptr, &mPool), "vkCreateDescriptorPool");

            const VkDescriptorSetLayout named = layout.getHandle();
            const VkDescriptorSetAllocateInfo allocate{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &named,
            };
            checkVk(vkAllocateDescriptorSets(mDevice.getHandle(), &allocate, &mSet), "vkAllocateDescriptorSets");

            const std::array<const Image*, sChannels> every = everyChannel();

            std::array<VkDescriptorImageInfo, sChannels> views{};
            std::array<VkWriteDescriptorSet, sChannels> writes{};
            for (std::uint32_t channel = 0; channel < sChannels; ++channel)
            {
                views[channel]
                    = VkDescriptorImageInfo{ VK_NULL_HANDLE, every[channel]->getView(), VK_IMAGE_LAYOUT_GENERAL };
                writes[channel] = VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = mSet,
                    .dstBinding = channel,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &views[channel],
                };
            }

            vkUpdateDescriptorSets(mDevice.getHandle(), sChannels, writes.data(), 0, nullptr);
        }
        catch (...)
        {
            destroy();
            throw;
        }
    }

    GBuffer::~GBuffer()
    {
        destroy();
    }

    void GBuffer::destroy()
    {
        // The set goes with the pool it came out of, which is what one pool per buffer is for.
        if (mPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(mDevice.getHandle(), mPool, nullptr);
    }

    std::array<const Image*, GBuffer::sChannels> GBuffer::everyChannel() const
    {
        // **Placed by name and not by position.** The binding a channel is written to is its index
        // here, and an initializer list said that only by the order somebody happened to write it
        // in — so reordering the list rebound every channel, which compiles, runs, and hands each
        // pass a different image than it declared.
        std::array<const Image*, sChannels> every{};
        every[Shaders::CHANNEL_DIRECT] = &mDirect;
        every[Shaders::CHANNEL_INDIRECT] = &mIndirect;
        every[Shaders::CHANNEL_ALBEDO] = &mAlbedo;
        every[Shaders::CHANNEL_SPECULAR] = &mSpecular;
        every[Shaders::CHANNEL_GUIDE] = &mGuide;
        every[Shaders::CHANNEL_MOTION] = &mMotion;
        every[Shaders::CHANNEL_DEPTH] = &mDepth;
        every[Shaders::CHANNEL_REFLECTION_MOTION] = &mReflectionMotion;
        every[Shaders::CHANNEL_PARTICLE_MASK] = &mParticleMask;
        every[Shaders::CHANNEL_BIAS_MASK] = &mBiasMask;
        every[Shaders::CHANNEL_STARS_SHOWN] = &mStarsShown;
        every[Shaders::CHANNEL_TRANSPARENCY] = &mTransparency;
        every[Shaders::CHANNEL_TRANSPARENCY_OPACITY] = &mTransparencyOpacity;
        every[Shaders::CHANNEL_TRANSPARENCY_MOTION] = &mTransparencyMotion;

        // A channel this forgot is a null the sweeps below would walk off, and nothing else says so.
        assert(std::find(every.begin(), every.end(), nullptr) == every.end() && "a channel slot the list did not fill");

        return every;
    }

    void GBuffer::begin(VkCommandBuffer commands) const
    {
        // From undefined, because every pixel of all of them is written before any is read and there is
        // nothing in them worth carrying across a frame. Keeping the old contents would cost a
        // decompress on some hardware and buy a guarantee nothing here wants.
        //
        // **But waiting on the last frame's readers, which is not the same thing as discarding.**
        // One set of channels serves every frame, and two are in flight — so the trace that is
        // about to overwrite these may start while the composite, the curve or the upscaler reading
        // them for the previous frame is still running. An execution dependency is the whole of
        // what a write-after-read needs; nothing has to be made visible, only ordered. Sourced at
        // everything before it on the queue rather than at the compute stage, because what NGX
        // reads them at is its own; discarding from `TOP_OF_PIPE` waits for nothing at all, and
        // buys a torn frame for a barrier saved.
        for (const Image* image : everyChannel())
            image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    void GBuffer::handOver(VkCommandBuffer commands) const
    {
        // **A read after a write, and nothing more.** Nothing after the trace writes a channel —
        // the accumulator blends into an image of its own — so every one of these is read-only from
        // here to the end of the frame.
        for (const Image* image : everyChannel())
            image->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    SetLayout GBuffer::describeLayout(const Device& device)
    {
        // Every channel is a storage image the trace writes, and they are bound one per number from
        // nought — which is what `gbuffer.h`'s `CHANNEL_*` are, so nothing here has to name them.
        //
        // **Both stages, because both kinds of pass are handed this set.** The trace is a launch and
        // everything that reads what it left — the accumulator, the wavelet, the composite — is a
        // dispatch.
        std::array<VkDescriptorSetLayoutBinding, sChannels> bindings{};
        for (std::uint32_t channel = 0; channel < bindings.size(); ++channel)
            bindings[channel] = VkDescriptorSetLayoutBinding{ channel, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };

        return SetLayout(device, bindings);
    }
}
