#include "wavepass.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>

#include <osg/Vec2f>

#include <components/rtx/shaders/gbuffer.h>

#include "commands.hpp"
#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// The amplitudes, how fast each turns, and the three packed fields between them.
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> sFormBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };

        constexpr std::array<VkDescriptorSetLayoutBinding, 1> sLineBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };

        /// The fields in, and the three textures out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 4> sComposeBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };

        /// How many complex numbers the transform runs over for one tile: three packed fields, each
        /// the grid itself.
        std::size_t fieldOf(std::size_t grid)
        {
            return 3 * grid * grid;
        }

        /// A chain down to one texel, which is what makes the last level the tile's own mean.
        std::uint32_t levelsFor(std::size_t grid)
        {
            return static_cast<std::uint32_t>(std::bit_width(grid));
        }

        std::uint32_t groupsFor(std::uint32_t extent)
        {
            return (extent + 7) / 8;
        }

        VkWriteDescriptorSet storedAt(std::uint32_t binding, const VkDescriptorImageInfo& image)
        {
            return VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = binding,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &image,
            };
        }

        VkWriteDescriptorSet blockAt(std::uint32_t binding, const VkDescriptorBufferInfo& block)
        {
            return VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = binding,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &block,
            };
        }
    }

    WavePass::WavePass(const Device& device, CommandPool& pool, const std::filesystem::path& shaderDirectory)
        : mDevice(device)
        , mPool(pool)
        , mFormPipeline(device, sFormBindings, sizeof(Shaders::WaveFormConstants), {},
              shaderDirectory / "waveform.comp.spv", "wave form")
        , mLinePipeline(device, sLineBindings, sizeof(Shaders::WaveConstants), {},
              shaderDirectory / "waveline.comp.spv", "wave line")
        , mComposePipeline(device, sComposeBindings, sizeof(Shaders::WaveComposeConstants), {},
              shaderDirectory / "wavecompose.comp.spv", "wave compose")
    {
        // After the pipelines, for the reason `BloomPass` gives: a member that throws while being
        // constructed leaves the ones already built to their own destructors, and a handle made in
        // this body would have none.
        const VkSamplerCreateInfo sampler{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        checkVk(vkCreateSampler(mDevice.getHandle(), &sampler, nullptr, &mSampler), "vkCreateSampler");

        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            Tile& tile = mTiles[index];
            const std::uint32_t grid = static_cast<std::uint32_t>(sWaveTiles[index].mGrid);
            const std::uint32_t levels = levelsFor(sWaveTiles[index].mGrid);

            tile.mField = Buffer(mDevice, fieldOf(sWaveTiles[index].mGrid) * 2 * sizeof(float),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            tile.mSurface = std::make_unique<Image>(
                mDevice, grid, grid, GBUFFER_ALBEDO, usage, std::format("wave surface {}", index), levels);
            tile.mCurvature = std::make_unique<Image>(
                mDevice, grid, grid, GBUFFER_ALBEDO, usage, std::format("wave curvature {}", index), levels);
            tile.mVariance = std::make_unique<Image>(
                mDevice, grid, grid, VK_FORMAT_R16_SFLOAT, usage, std::format("wave variance {}", index), levels);
        }

        describe(SeaState{});
    }

    WavePass::~WavePass()
    {
        if (mSampler != VK_NULL_HANDLE)
            vkDestroySampler(mDevice.getHandle(), mSampler, nullptr);
    }

    void WavePass::describe(const SeaState& sea)
    {
        if (mDrawn && mSea == sea)
            return;

        const std::array<WaveCascade, Shaders::WAVE_CASCADES> cascades = makeWaveCascades(sea);

        mSlope = waveSlope(cascades);

        Batch batch(mPool);
        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            mTiles[index].mAmplitudes = uploadBuffer(mDevice, batch,
                std::span<const osg::Vec2f>(cascades[index].mAmplitudes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            mTiles[index].mFrequencies = uploadBuffer(mDevice, batch,
                std::span<const float>(cascades[index].mFrequencies), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        batch.flush();

        mSea = sea;
        mDrawn = true;
    }

    void WavePass::handOver(VkCommandBuffer commands) const
    {
        const VkMemoryBarrier2 between{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &between,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    void WavePass::transform(VkCommandBuffer commands, const Tile& tile, std::uint32_t count) const
    {
        const VkDescriptorBufferInfo field{ tile.mField.getHandle(), 0, VK_WHOLE_SIZE };
        const VkWriteDescriptorSet write = blockAt(0, field);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mLinePipeline.getHandle());
        vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mLinePipeline.getLayout(), 0, 1, &write);

        // Three packed fields, each transformed along its rows and then along its columns — which is
        // the same shader with its two strides swapped, because a separable transform is the
        // one-dimensional one run twice.
        for (std::uint32_t pair = 0; pair < 3; ++pair)
            for (int pass = 0; pass < 2; ++pass)
            {
                const Shaders::WaveConstants along{
                    .mCount = count,
                    .mStride = pass == 0 ? 1u : count,
                    .mJump = pass == 0 ? count : 1u,
                    .mOffset = pair * count * count,
                };

                vkCmdPushConstants(
                    commands, mLinePipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(along), &along);
                vkCmdDispatch(commands, count, 1, 1);
                handOver(commands);
            }
    }

    void WavePass::record(VkCommandBuffer commands, float seconds) const
    {
        assert(mDrawn && "a frame before any sea state was described");

        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            const Tile& tile = mTiles[index];
            const std::uint32_t grid = static_cast<std::uint32_t>(sWaveTiles[index].mGrid);

            // Every level is written whole below, so none needs what the last frame left in it.
            for (const Image* image : { tile.mSurface.get(), tile.mCurvature.get(), tile.mVariance.get() })
                image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            const std::array<VkDescriptorBufferInfo, 3> blocks{
                VkDescriptorBufferInfo{ tile.mAmplitudes.getHandle(), 0, VK_WHOLE_SIZE },
                VkDescriptorBufferInfo{ tile.mFrequencies.getHandle(), 0, VK_WHOLE_SIZE },
                VkDescriptorBufferInfo{ tile.mField.getHandle(), 0, VK_WHOLE_SIZE },
            };
            const std::array<VkWriteDescriptorSet, 3> forms{
                blockAt(0, blocks[0]),
                blockAt(1, blocks[1]),
                blockAt(2, blocks[2]),
            };
            const Shaders::WaveFormConstants shaped{
                .mCount = grid,
                .mExtent = sWaveTiles[index].mExtent,
                .mTime = seconds,
            };

            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mFormPipeline.getHandle());
            vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mFormPipeline.getLayout(), 0,
                static_cast<std::uint32_t>(forms.size()), forms.data());
            vkCmdPushConstants(
                commands, mFormPipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shaped), &shaped);
            vkCmdDispatch(commands, groupsFor(grid), groupsFor(grid), 1);
            handOver(commands);

            transform(commands, tile, grid);

            const std::array<VkDescriptorImageInfo, 3> images{
                VkDescriptorImageInfo{ VK_NULL_HANDLE, tile.mSurface->getStorageView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, tile.mCurvature->getStorageView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, tile.mVariance->getStorageView(), VK_IMAGE_LAYOUT_GENERAL },
            };
            const std::array<VkWriteDescriptorSet, 4> composes{
                blockAt(0, blocks[2]),
                storedAt(1, images[0]),
                storedAt(2, images[1]),
                storedAt(3, images[2]),
            };
            const Shaders::WaveComposeConstants unpacked{ .mCount = grid };

            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mComposePipeline.getHandle());
            vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mComposePipeline.getLayout(), 0,
                static_cast<std::uint32_t>(composes.size()), composes.data());
            vkCmdPushConstants(
                commands, mComposePipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(unpacked), &unpacked);
            vkCmdDispatch(commands, groupsFor(grid), groupsFor(grid), 1);

            for (const Image* image : { tile.mSurface.get(), tile.mCurvature.get(), tile.mVariance.get() })
                image->buildMips(commands);
        }
    }
}
