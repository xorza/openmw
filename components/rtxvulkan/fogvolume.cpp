#include "fogvolume.hpp"

#include <array>
#include <cstddef>

#include <components/rtx/shaders/scene.h>

#include "commands.hpp"
#include "device.hpp"
#include "result.hpp"
#include "shaders/fogvolume.h"

namespace Rtx
{
    namespace
    {
        constexpr VkFormat sFormat = FOG_VOLUME_FORMAT;

        constexpr VkImageUsageFlags sUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        constexpr std::uint32_t sBindings = FogBindings::FOG_BINDING_COUNT;

        constexpr std::uint32_t sSampled = FogBindings::FOG_SAMPLED_COUNT;

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
        std::array<VkDescriptorSetLayoutBinding, sBindings> bindings{};
        for (std::uint32_t binding = 0; binding < bindings.size(); ++binding)
            bindings[binding] = VkDescriptorSetLayoutBinding{ binding,
                sampledAt(binding) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
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
        , mCoverage{ Image(device, mColumns, mRows, FOG_COVERAGE_FORMAT, sUsage, "fog coverage 0", 1,
                         Shaders::FOG_VOLUME_SLICES),
            Image(
                device, mColumns, mRows, FOG_COVERAGE_FORMAT, sUsage, "fog coverage 1", 1, Shaders::FOG_VOLUME_SLICES) }
        , mVisibility{ Image(
                           device, mColumns, mRows, sFormat, sUsage, "fog visibility 0", 1, Shaders::FOG_VOLUME_SLICES),
            Image(device, mColumns, mRows, sFormat, sUsage, "fog visibility 1", 1, Shaders::FOG_VOLUME_SLICES) }
        , mLamps(device, mColumns, mRows, sFormat, sUsage, "fog lamps", 1, Shaders::FOG_VOLUME_SLICES)
        , mSlice(device, mColumns, mRows, sFormat, sUsage, "fog slice", 1, Shaders::FOG_VOLUME_SLICES)
        , mSliceVisibility(device, mColumns, mRows, FOG_DIRECTIONAL_FORMAT, sUsage, "fog slice visibility", 1,
              Shaders::FOG_VOLUME_SLICES)
        , mColumnDepth(device, mColumns, mRows, FOG_DEPTH_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT, "fog column depth")
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

            for (std::size_t parity = 0; parity < mSets.size(); ++parity)
            {
                const std::size_t written = parity;
                const std::size_t history = 1 - parity;

                std::array<const Image*, sBindings> named{};
                named[FogBindings::BIND_FOG_WAS_COVERAGE] = &mCoverage[history];
                named[FogBindings::BIND_FOG_WAS_VISIBILITY] = &mVisibility[history];
                named[FogBindings::BIND_FOG_COVERAGE] = &mCoverage[written];
                named[FogBindings::BIND_FOG_VISIBILITY] = &mVisibility[written];
                named[FogBindings::BIND_FOG_LAMPS] = &mLamps;
                named[FogBindings::BIND_FOG_SLICE] = &mSlice;
                named[FogBindings::BIND_FOG_SLICE_VISIBILITY] = &mSliceVisibility;
                named[FogBindings::BIND_FOG_COVERAGE_TARGET] = &mCoverage[written];
                named[FogBindings::BIND_FOG_VISIBILITY_TARGET] = &mVisibility[written];
                named[FogBindings::BIND_FOG_LAMPS_TARGET] = &mLamps;
                named[FogBindings::BIND_FOG_SLICE_TARGET] = &mSlice;
                named[FogBindings::BIND_FOG_SLICE_VISIBILITY_TARGET] = &mSliceVisibility;
                named[FogBindings::BIND_FOG_COLUMN_DEPTH] = &mColumnDepth;

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
                for (const Image* image : { &mCoverage[0], &mCoverage[1], &mVisibility[0], &mVisibility[1], &mLamps,
                         &mSlice, &mSliceVisibility, &mColumnDepth })
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

        for (const Image* image :
            { &mCoverage[written], &mVisibility[written], &mLamps, &mSlice, &mSliceVisibility, &mColumnDepth })
            image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        for (const Image* image : { &mCoverage[1 - written], &mVisibility[1 - written] })
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

        for (const Image* image : { &mCoverage[written], &mVisibility[written], &mLamps })
            image->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void FogVolume::handOver(VkCommandBuffer commands) const
    {
        for (const Image* image : { &mSlice, &mSliceVisibility })
            image->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
}
