#include "gbuffer.hpp"
#include <algorithm>
#include <cassert>

#include <components/rtx/shaders/gbuffer.h>

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

        /// The guide is full floats so that a normal stays a normal after three of its components
        /// have been quantised; the roughness beside it would fit in anything.
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

        /// One float for a value that is a yes or a no and a value between nought and one. See
        /// `gbuffer.h` for why it is not a byte.
        constexpr VkFormat sMask = GBUFFER_MASK;

        /// **`SAMPLED` on all of them, and it is not decoration.** DLSS samples every input it is
        /// handed; one without the bit reads as zero, NGX returns success and the validation layers
        /// say nothing, so the whole frame comes back black with nothing pointing at the cause. It
        /// costs no memory, so every channel carries it rather than only the five DLSS reads today.
        constexpr VkImageUsageFlags sUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        /// The channels a caller can ask to read back: the bounce, the two motion fields, the depth
        /// and the two masks. See `Rtx::Channel`.
        constexpr VkImageUsageFlags sReadable = sUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    GBuffer::GBuffer(const Device& device, std::uint32_t width, std::uint32_t height)
        : mDirect(device, width, height, sRadiance, sUsage, "g-direct")
        , mIndirect(device, width, height, sRadiance, sReadable, "g-indirect")
        , mAlbedo(device, width, height, sAlbedo, sUsage, "g-albedo")
        , mSpecular(device, width, height, sAlbedo, sUsage, "g-specular")
        , mGuide(device, width, height, sGuide, sUsage, "g-guide")
        , mMotion(device, width, height, sMotion, sReadable, "g-motion")
        , mDepth(device, width, height, sDepth, sReadable, "g-depth")
        , mReflectionMotion(device, width, height, sMotion, sReadable, "g-reflection-motion")
        , mParticleMask(device, width, height, sMask, sReadable, "g-particle-mask")
        , mBiasMask(device, width, height, sMask, sReadable, "g-bias-mask")
    {
    }

    std::array<const Image*, GBuffer::sChannels> GBuffer::everyChannel() const
    {
        const std::array<const Image*, sChannels> every{ &mDirect, &mIndirect, &mAlbedo, &mSpecular, &mGuide, &mMotion,
            &mDepth, &mReflectionMotion, &mParticleMask, &mBiasMask };

        // **Too many is a compiler error and too few is not**, which is the direction that hurts: an
        // aggregate short of its size fills the rest with null and every sweep below then walks off
        // one. Naming one more than the list has says so at build time; naming one fewer says so
        // here.
        assert(std::find(every.begin(), every.end(), nullptr) == every.end() && "a channel slot the list did not fill");

        return every;
    }

    void GBuffer::begin(VkCommandBuffer commands) const
    {
        // From undefined, because every pixel of all of them is written before any is read and there is
        // nothing in them worth carrying across a frame. Keeping the old contents would cost a
        // decompress on some hardware and buy a guarantee nothing here wants.
        //
        // **But waiting on the last frame's composite, which is not the same thing as discarding.**
        // One set of channels serves every frame, and a window keeps two in flight — so the trace
        // that is about to overwrite these may start while the composite reading them for the
        // previous frame is still running. Sourcing the barrier at the compute stage is the whole of
        // what a write-after-read needs; nothing has to be made visible, only ordered. Discarding
        // from `TOP_OF_PIPE` waits for nothing at all, and buys a torn frame for a barrier saved.
        for (const Image* image : everyChannel())
            image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    void GBuffer::handOver(VkCommandBuffer commands) const
    {
        for (const Image* image : everyChannel())
            image->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
}
