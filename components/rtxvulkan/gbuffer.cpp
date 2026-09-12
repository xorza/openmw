#include "gbuffer.hpp"

#include <algorithm>
#include <array>

#include <components/rtx/shaders/gbuffer.h>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// Full floats for the three radiance channels, and not halves: a reference is a sum of a
        /// thousand frames, and rounding every term before adding it only averages away if the
        /// error is random, which it is not — `direct` is all but identical from frame to frame,
        /// and the sampler is a low-discrepancy sequence. In halves, the converged mean of a flat
        /// surface comes out low by more than the test's tolerance.
        constexpr VkFormat sRadiance = GBUFFER_RADIANCE;

        /// Half floats, and `gbuffer.h` has the angles the width is derived from. A normal is
        /// compared against a neighbour's and thrown away, never summed.
        constexpr VkFormat sGuide = GBUFFER_GUIDE;

        /// Half floats, because an albedo is a fraction. The diffuse albedo takes it too, which
        /// needed measuring: quantising a per-pixel constant is a systematic error on the indirect
        /// term, but on a converged reference of a room the mean moves by a fiftieth of the
        /// tolerance the radiance channels are held to.
        constexpr VkFormat sAlbedo = GBUFFER_ALBEDO;

        /// Two halves, for the reason `gbuffer.h` gives.
        constexpr VkFormat sMotion = GBUFFER_MOTION;

        /// Two, and full floats rather than halves: a clip depth has little precision left at the
        /// far end of a Morrowind view, and the distance beside it runs past thirty thousand units
        /// where a half's steps are thirty-two units wide. NGX reads the first and is handed the
        /// pair, which costs it nothing measurable.
        constexpr VkFormat sDepth = GBUFFER_DEPTH;

        /// Half floats for the layer the eye sees through: nothing sums it, and a reference is
        /// built with the upscaler off.
        constexpr VkFormat sLayer = GBUFFER_LAYER;

        /// Three channels for one number, because a single-channel image handed to
        /// `pInTransparencyLayerOpacity` is read as a colour: a coverage in red alone turned grey
        /// smoke over a blue sky cyan. A byte apiece because a coverage is a fraction.
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

        /// The channels a caller can ask to read back: the bounce, the three motion fields, the
        /// depth and the two masks. See `Rtx::Channel`.
        constexpr VkImageUsageFlags sReadable = sUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        struct ChannelFormat
        {
            VkFormat mFormat;
            VkImageUsageFlags mUsage;
        };

        /// What each channel is made of, at its own binding, placed by name so a channel added to
        /// `Rtx::Channel` and forgotten here is a compile error rather than an image bound at the
        /// wrong number.
        const ChannelFormat& formatOf(const Channel channel)
        {
            static constexpr auto sFormats = [] {
                std::array<ChannelFormat, sChannelCount> every{};
                every[bindingOf(Channel::Direct)] = { sRadiance, sUsage };
                every[bindingOf(Channel::Indirect)] = { sRadiance, sReadable };
                every[bindingOf(Channel::Albedo)] = { sAlbedo, sUsage };
                every[bindingOf(Channel::Specular)] = { sAlbedo, sUsage };
                every[bindingOf(Channel::Guide)] = { sGuide, sUsage };
                every[bindingOf(Channel::Motion)] = { sMotion, sReadable };
                every[bindingOf(Channel::Depth)] = { sDepth, sReadable };
                every[bindingOf(Channel::ReflectionMotion)] = { sMotion, sReadable };
                every[bindingOf(Channel::ParticleMask)] = { sMask, sReadable };
                every[bindingOf(Channel::BiasMask)] = { sMask, sReadable };
                every[bindingOf(Channel::StarsShown)] = { sStars, sUsage };
                every[bindingOf(Channel::Transparency)] = { sLayer, sUsage };
                every[bindingOf(Channel::TransparencyOpacity)] = { sLayerOpacity, sUsage };
                every[bindingOf(Channel::TransparencyMotion)] = { sMotion, sReadable };

                return every;
            }();

            static_assert(std::ranges::none_of(sFormats, [](const ChannelFormat& one) { return one.mUsage == 0; }),
                "a channel the format table did not fill");

            return sFormats[bindingOf(channel)];
        }
    }

    GBuffer::GBuffer(const Device& device, CommandPool& pool, const SetLayout& layout, const std::uint32_t width,
        const std::uint32_t height, const bool layers)
        : mCarried(layers ? sChannelCount : bindingOf(Channel::Transparency))
    {
        // **The three the eye sees through are last, so one count says which are the frame's.**
        // `sEveryChannel` is in binding order and `gbuffer.h` puts them at the end.
        static_assert(
            bindingOf(Channel::Transparency) + 3 == sChannelCount, "the layer channels are no longer the last three");

        mChannels.reserve(sChannelCount);
        for (const Channel channel : sEveryChannel)
        {
            const ChannelFormat& described = formatOf(channel);

            // One texel where nothing will read the channel, which is sixteen bytes a pixel of the
            // frame; a store outside an image is discarded by the specification, and
            // `visibility.rgen` writes these three only under `mLayerCompositedAfter` anyway.
            if (carries(channel))
                mChannels.emplace_back(
                    device, width, height, described.mFormat, described.mUsage, channelName(channel));
            else
                mChannels.push_back(
                    makeStandIn(device, pool, described.mFormat, VK_IMAGE_USAGE_STORAGE_BIT, channelName(channel)));
        }

        const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sChannelCount };
        const VkDescriptorPoolCreateInfo describePool{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &size,
        };
        checkVk(vkCreateDescriptorPool(device.getHandle(), &describePool, nullptr, mPool.put(device.getHandle())),
            "vkCreateDescriptorPool");

        const VkDescriptorSetLayout named = layout.get();
        const VkDescriptorSetAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = mPool.get(),
            .descriptorSetCount = 1,
            .pSetLayouts = &named,
        };
        checkVk(vkAllocateDescriptorSets(device.getHandle(), &allocate, &mSet), "vkAllocateDescriptorSets");

        std::array<VkDescriptorImageInfo, sChannelCount> views{};
        std::array<VkWriteDescriptorSet, sChannelCount> writes{};
        for (std::uint32_t channel = 0; channel < sChannelCount; ++channel)
        {
            views[channel]
                = VkDescriptorImageInfo{ VK_NULL_HANDLE, mChannels[channel].getView(), VK_IMAGE_LAYOUT_GENERAL };
            writes[channel] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = mSet,
                .dstBinding = channel,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &views[channel],
            };
        }

        vkUpdateDescriptorSets(device.getHandle(), sChannelCount, writes.data(), 0, nullptr);
    }

    void GBuffer::begin(VkCommandBuffer commands) const
    {
        // From undefined, because every pixel is written before any is read — but waiting on the
        // last frame's readers, because one set of channels serves every frame and two are in
        // flight. Sourced at everything before it on the queue rather than at the compute stage,
        // because what NGX reads them at is its own. The stand-ins are in here too
        // (`CompositePass::mNoSum`).
        Barriers barriers(commands);
        for (const Image& image : mChannels)
            barriers.add(
                image.describeTransition(ImageUse{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT },
                    ImageUse{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT }));

        barriers.flush();
    }

    void GBuffer::handOver(VkCommandBuffer commands) const
    {
        // A read after a write, and nothing more: every channel is read-only from here to the end
        // of the frame. Sampled as well as loaded, because DLSS samples every guide it is handed.
        Barriers barriers(commands);
        for (const Image& image : mChannels)
            barriers.add(image.describeTransition(
                ImageUse{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
                ImageUse{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT }));

        barriers.flush();
    }

    SetLayout GBuffer::describeLayout(const Device& device)
    {
        // Every channel is a storage image the trace writes, bound one per number from nought,
        // which is what `gbuffer.h`'s `CHANNEL_*` are. Both stages, because the trace is a launch
        // and everything that reads what it left is a dispatch.
        std::array<VkDescriptorSetLayoutBinding, sChannelCount> bindings{};
        for (std::uint32_t channel = 0; channel < bindings.size(); ++channel)
            bindings[channel] = VkDescriptorSetLayoutBinding{ channel, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };

        return makeSetLayout(device, bindings);
    }
}
