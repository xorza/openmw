#include "fogvolume.hpp"

#include <array>

#include <components/rtx/shaders/scene.h>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// Half floats, and the range is what makes them enough.
        ///
        /// **Neither image holds a quantity that grows.** The air's `rgb` is an in-scatter the
        /// transmittance beside it bounds, and its `a` runs from one down to nought; the sun's is a
        /// product of transmittances, so it never leaves the unit interval — the irradiance and the
        /// phase that would take it anywhere else are exactly the two factors the trace puts back.
        /// So the argument `GBuffer` makes for full floats on a channel a reference accumulates a
        /// thousand frames into does not reach here: nothing sums these.
        constexpr VkFormat sFormat = FOG_VOLUME_FORMAT;

        constexpr VkImageUsageFlags sUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        std::uint32_t columnsFor(std::uint32_t pixels)
        {
            return (pixels + Shaders::FOG_VOLUME_SCALE - 1) / Shaders::FOG_VOLUME_SCALE;
        }
    }

    FogVolumeLayout::FogVolumeLayout(const Device& device)
        : mDevice(device)
    {
        // The two the trace samples, then the two the volume pass writes. One image is named twice
        // because the two accesses want different descriptors and Vulkan has no one descriptor that
        // is both.
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (std::uint32_t binding = 0; binding < bindings.size(); ++binding)
            bindings[binding] = VkDescriptorSetLayoutBinding{ binding,
                binding < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

        const VkDescriptorSetLayoutCreateInfo describe{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<std::uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };
        checkVk(vkCreateDescriptorSetLayout(mDevice.getHandle(), &describe, nullptr, &mHandle),
            "vkCreateDescriptorSetLayout");
    }

    FogVolumeLayout::~FogVolumeLayout()
    {
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(mDevice.getHandle(), mHandle, nullptr);
    }

    FogVolume::FogVolume(const Device& device, const FogVolumeLayout& layout, std::uint32_t width, std::uint32_t height)
        : mDevice(device)
        , mColumns(columnsFor(width))
        , mRows(columnsFor(height))
        , mAir(device, mColumns, mRows, sFormat, sUsage, "fog air", 1, Shaders::FOG_VOLUME_SLICES)
        , mSunward(device, mColumns, mRows, sFormat, sUsage, "fog sunward", 1, Shaders::FOG_VOLUME_SLICES)
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

            const std::array<VkDescriptorPoolSize, 2> sizes{
                VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
                VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
            };
            const VkDescriptorPoolCreateInfo describePool{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 1,
                .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
                .pPoolSizes = sizes.data(),
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

            // **Sampled from `GENERAL` rather than moved to a read-only layout**, for the reason
            // `BloomPass` gives: these are written as storage images and read as sampled ones a
            // dispatch apart, and `GENERAL` is the one layout both accesses are legal from.
            const std::array<VkDescriptorImageInfo, 4> views{
                VkDescriptorImageInfo{ mSampler, mAir.getView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ mSampler, mSunward.getView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, mAir.getStorageView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, mSunward.getStorageView(), VK_IMAGE_LAYOUT_GENERAL },
            };

            std::array<VkWriteDescriptorSet, 4> writes{};
            for (std::uint32_t binding = 0; binding < writes.size(); ++binding)
                writes[binding] = VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = mSet,
                    .dstBinding = binding,
                    .descriptorCount = 1,
                    .descriptorType
                    = binding < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &views[binding],
                };

            vkUpdateDescriptorSets(
                mDevice.getHandle(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
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

    void FogVolume::begin(VkCommandBuffer commands) const
    {
        for (const Image* image : { &mAir, &mSunward })
            image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    void FogVolume::handOver(VkCommandBuffer commands) const
    {
        for (const Image* image : { &mAir, &mSunward })
            image->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
}
