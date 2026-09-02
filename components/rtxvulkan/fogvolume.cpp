#include "fogvolume.hpp"

#include <array>

#include <components/rtx/shaders/scene.h>

#include "commands.hpp"
#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// Half floats, and the range is what makes them enough.
        ///
        /// **Nothing here holds a quantity that grows.** A point's scattering is a radiance the
        /// weather sets and its extinction is per world unit; the sun's is a product of
        /// transmittances, so it never leaves the unit interval — the irradiance and the phase that
        /// would take it anywhere else are exactly the two factors the trace puts back. The
        /// integrated pair is bounded by the transmittance beside it. So the argument `GBuffer`
        /// makes for full floats on a channel a reference accumulates a thousand frames into does
        /// not reach here: nothing sums these.
        constexpr VkFormat sFormat = FOG_VOLUME_FORMAT;

        constexpr VkImageUsageFlags sUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        /// Everything a pass reads, and then everything a pass writes. `lib/bindings.glsl` names
        /// them in this order.
        ///
        /// **Eight images and sixteen of these are seven of them named twice**, because Vulkan has
        /// no descriptor a shader may both sample and store through — and every one of them but the
        /// history is written by one pass and read by the next. The column depth is the eighth,
        /// named once: both passes reach it through the one storage binding.
        constexpr std::uint32_t sBindings = 17;

        /// Whether a binding is read or written, which the layout and the writes must not disagree
        /// about. Sampled first and storage after, so this is a bound rather than a table.
        constexpr std::uint32_t sSampled = 9;

        constexpr bool sampledAt(std::uint32_t binding)
        {
            return binding < sSampled;
        }

        std::uint32_t columnsFor(std::uint32_t pixels)
        {
            return (pixels + Shaders::FOG_VOLUME_SCALE - 1) / Shaders::FOG_VOLUME_SCALE;
        }
    }

    SetLayout FogVolume::describeLayout(const Device& device)
    {
        // Sampled where a pass reads and storage where it writes. One image is named twice wherever
        // both happen, because Vulkan has no one descriptor that is both.
        std::array<VkDescriptorSetLayoutBinding, sBindings> bindings{};
        for (std::uint32_t binding = 0; binding < bindings.size(); ++binding)
            bindings[binding] = VkDescriptorSetLayoutBinding{ binding,
                sampledAt(binding) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                // The volume writes these as a dispatch, and the trace samples them from its ray
                // generation shader and from the closest-hit shaders that shade what a bounce found.
                VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                    | VK_SHADER_STAGE_MISS_BIT_KHR,
                nullptr };

        return SetLayout(device, bindings);
    }

    FogVolume::FogVolume(
        const Device& device, CommandPool& pool, const SetLayout& layout, std::uint32_t width, std::uint32_t height)
        : mDevice(device)
        , mColumns(columnsFor(width))
        , mRows(columnsFor(height))
        , mScatter{ Image(device, mColumns, mRows, sFormat, sUsage, "fog scatter 0", 1, Shaders::FOG_VOLUME_SLICES),
            Image(device, mColumns, mRows, sFormat, sUsage, "fog scatter 1", 1, Shaders::FOG_VOLUME_SLICES) }
        , mSunward{ Image(device, mColumns, mRows, sFormat, sUsage, "fog sunward 0", 1, Shaders::FOG_VOLUME_SLICES),
            Image(device, mColumns, mRows, sFormat, sUsage, "fog sunward 1", 1, Shaders::FOG_VOLUME_SLICES) }
        , mLamps(device, mColumns, mRows, sFormat, sUsage, "fog lamps", 1, Shaders::FOG_VOLUME_SLICES)
        , mAir(device, mColumns, mRows, sFormat, sUsage, "fog air", 1, Shaders::FOG_VOLUME_SLICES)
        , mAirSunward(device, mColumns, mRows, sFormat, sUsage, "fog air sunward", 1, Shaders::FOG_VOLUME_SLICES)
        , mSlice(device, mColumns, mRows, sFormat, sUsage, "fog slice", 1, Shaders::FOG_VOLUME_SLICES)
        , mSliceSunward(device, mColumns, mRows, sFormat, sUsage, "fog slice sunward", 1, Shaders::FOG_VOLUME_SLICES)
        , mColumnDepth(device, mColumns, mRows, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "fog column depth")
    {
        try
        {
            const VkSamplerCreateInfo describeSampler{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_LINEAR,
                .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            };
            checkVk(vkCreateSampler(mDevice.getHandle(), &describeSampler, nullptr, &mSampler), "vkCreateSampler");

            const auto sets = static_cast<std::uint32_t>(mSets.size());
            const std::array<VkDescriptorPoolSize, 2> sizes{
                VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sSampled * sets },
                VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (sBindings - sSampled) * sets },
            };
            const VkDescriptorPoolCreateInfo describePool{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = sets,
                .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
                .pPoolSizes = sizes.data(),
            };
            checkVk(
                vkCreateDescriptorPool(mDevice.getHandle(), &describePool, nullptr, &mPool), "vkCreateDescriptorPool");

            const std::array<VkDescriptorSetLayout, 2> shapes{ layout.getHandle(), layout.getHandle() };
            const VkDescriptorSetAllocateInfo allocate{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mPool,
                .descriptorSetCount = static_cast<std::uint32_t>(shapes.size()),
                .pSetLayouts = shapes.data(),
            };
            checkVk(vkAllocateDescriptorSets(mDevice.getHandle(), &allocate, mSets.data()), "vkAllocateDescriptorSets");

            // **Sampled from `GENERAL` rather than moved to a read-only layout**, for the reason
            // `BloomPass` gives: these are written as storage images and read as sampled ones a
            // dispatch apart, and `GENERAL` is the one layout both accesses are legal from.
            for (std::size_t parity = 0; parity < mSets.size(); ++parity)
            {
                const std::size_t written = parity;
                const std::size_t history = 1 - parity;

                const std::array<const Image*, sBindings> named{
                    &mScatter[history],
                    &mSunward[history],
                    &mScatter[written],
                    &mSunward[written],
                    &mLamps,
                    &mAir,
                    &mAirSunward,
                    &mSlice,
                    &mSliceSunward,
                    &mScatter[written],
                    &mSunward[written],
                    &mLamps,
                    &mAir,
                    &mAirSunward,
                    &mSlice,
                    &mSliceSunward,
                    &mColumnDepth,
                };

                std::array<VkDescriptorImageInfo, sBindings> views{};
                std::array<VkWriteDescriptorSet, sBindings> writes{};
                for (std::uint32_t binding = 0; binding < sBindings; ++binding)
                {
                    views[binding] = VkDescriptorImageInfo{ sampledAt(binding) ? mSampler : VK_NULL_HANDLE,
                        sampledAt(binding) ? named[binding]->getView() : named[binding]->getStorageView(),
                        VK_IMAGE_LAYOUT_GENERAL };
                    writes[binding] = VkWriteDescriptorSet{
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = mSets[parity],
                        .dstBinding = binding,
                        .descriptorCount = 1,
                        .descriptorType = sampledAt(binding) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                             : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .pImageInfo = &views[binding],
                    };
                }

                vkUpdateDescriptorSets(mDevice.getHandle(), sBindings, writes.data(), 0, nullptr);
            }

            pool.submitAndWait([&](VkCommandBuffer commands) {
                for (const Image* image : { &mScatter[0], &mScatter[1], &mSunward[0], &mSunward[1], &mLamps, &mAir,
                         &mAirSunward, &mSlice, &mSliceSunward, &mColumnDepth })
                    image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            });
        }
        catch (...)
        {
            destroy();
            throw;
        }
    }

    FogVolume::~FogVolume()
    {
        destroy();
    }

    void FogVolume::destroy()
    {
        if (mPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(mDevice.getHandle(), mPool, nullptr);
        if (mSampler != VK_NULL_HANDLE)
            vkDestroySampler(mDevice.getHandle(), mSampler, nullptr);
    }

    void FogVolume::begin(VkCommandBuffer commands, std::uint64_t frame) const
    {
        const std::size_t written = writtenAt(frame);

        // **Discarded, because every texel of it is written before any is read.** Keeping what the
        // frame before last left here would cost a decompress and buy nothing. The other half of the
        // pair is this frame's history and survives, three loops down.
        for (const Image* image : { &mScatter[written], &mSunward[written], &mLamps })
            image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        // **Discarded like the three above, and read by the trace like the four below**: the pass
        // that finds it writes it, the two that fill and integrate the froxels read it, and the
        // trace reads it too, to carry a pixel's last slice the way the integrate pass did. Read
        // as storage everywhere, because nothing samples between two columns' depths.
        mColumnDepth.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        // **Sampled as well as written, because a frame that dispatches no air still binds these.**
        // An interior integrates its own even haze and runs no volume pass, so what follows this
        // barrier is the trace with the pair bound — `VisibilityPass::record` says why it is bound
        // either way. Named for the write alone, this covered only the frames with an air pass to
        // hand over.
        for (const Image* image : { &mAir, &mAirSunward, &mSlice, &mSliceSunward })
            image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        // **From `GENERAL` and not from undefined**, which is the whole of what makes a history a
        // history: the frame that wrote it two frames ago left it here, and discarding it would hand
        // this frame a volume of nothing to average against.
        for (const Image* image : { &mScatter[1 - written], &mSunward[1 - written] })
            image->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void FogVolume::depthTaken(VkCommandBuffer commands) const
    {
        mColumnDepth.transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    void FogVolume::scattered(VkCommandBuffer commands, const std::uint64_t frame) const
    {
        const std::size_t written = writtenAt(frame);

        // **`GENERAL` to `GENERAL`, which is what a barrier between two compute passes over one
        // image is**: the layout is right for both accesses already — `BloomPass` says why these are
        // never moved to a read-only one — so what this orders is the writes against the reads and
        // nothing else.
        for (const Image* image : { &mScatter[written], &mSunward[written], &mLamps })
            image->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void FogVolume::handOver(VkCommandBuffer commands) const
    {
        for (const Image* image : { &mAir, &mAirSunward, &mSlice, &mSliceSunward })
            image->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        // The column depth the trace reads beside them, which `depthTaken` ordered only against the
        // two compute passes between.
        mColumnDepth.transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
}
