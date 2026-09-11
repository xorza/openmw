#include "wavepass.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <utility>

#include <osg/Vec2f>

#include <components/rtx/shaders/gbuffer.h>

#include "commands.hpp"
#include "device.hpp"
#include "dispatch.hpp"
#include "graveyard.hpp"

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

        /// The side of the workgroup `wavecompose.comp` declares, which is what its dispatch has
        /// to be counted in.
        constexpr std::uint32_t sWaveWorkgroup = 8;

        /// How many complex numbers the transform runs over for one tile: three packed fields, each
        /// the grid itself.
        std::size_t fieldOf(std::size_t grid)
        {
            return 3 * grid * grid;
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
        , mSampler(Sampler::forContent(device, "wave"))
    {
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            Tile& tile = mTiles[index];
            const std::uint32_t grid = static_cast<std::uint32_t>(sWaveTiles[index].mGrid);
            const std::uint32_t levels = levelsFor(sWaveTiles[index].mGrid);

            tile.mField = Buffer::deviceLocal(
                mDevice, fieldOf(sWaveTiles[index].mGrid) * 2 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

            tile.mSurface = std::make_unique<Image>(
                mDevice, grid, grid, GBUFFER_ALBEDO, usage, std::format("wave surface {}", index), levels);
            tile.mCurvature = std::make_unique<Image>(
                mDevice, grid, grid, GBUFFER_ALBEDO, usage, std::format("wave curvature {}", index), levels);
        }

        // Nothing is in flight when the pass is made, so what this replaces is a set of buffers
        // that hold nothing — which `Graveyard::bury` takes and does nothing with.
        Graveyard nothing(mDevice, mPool);
        describe(SeaState{}, nothing);

        // **Every tile in the layout the trace binds it in, from the first frame.** A frame with no
        // water in it synthesises nothing and binds the tiles anyway, because the shader declares
        // them; an image that was never transitioned is in no layout at all, and a descriptor
        // naming it as `GENERAL` is an error whether or not a ray ever samples it. Synthesising
        // once here is what moves them, and leaves a sea rather than nothing in them.
        mPool.submitAndWait([&](VkCommandBuffer commands) { record(commands, 0.0f); });
    }

    void WavePass::describe(const SeaState& sea, Graveyard& graveyard)
    {
        if (mDrawn && mSea == sea)
            return;

        const std::array<WaveCascade, Shaders::WAVE_CASCADES> cascades = makeWaveCascades(sea);

        mSlope = waveSlope(cascades);
        mCurvature = waveCurvature(cascades);

        Batch batch(mPool);
        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            // **Buried and not dropped.** A frame in flight is still synthesising from the spectrum
            // this replaces, and a weather that turns the wind is what makes that happen: assigning
            // over these frees them where the queue has not reached the dispatch that reads them.
            graveyard.bury(std::exchange(mTiles[index].mAmplitudes,
                uploadBuffer(mDevice, batch, std::span<const osg::Vec2f>(cascades[index].mAmplitudes),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)));
            graveyard.bury(std::exchange(mTiles[index].mFrequencies,
                uploadBuffer(mDevice, batch, std::span<const float>(cascades[index].mFrequencies),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)));
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
            // The synthesis is a dispatch and the trace that samples what it left is a launch.
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,

            // **Written as well as read, because the transform runs in place.** Each of the six
            // passes over a field reads it and writes it back, so what follows one is a write after
            // a write as much as a read after one — and a dependency naming only the read leaves the
            // two writes unordered against each other. The execution order held either way, which is
            // why nothing was ever seen to go wrong.
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
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
        const VkWriteDescriptorSet write = bufferWrite(0, field);

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

            // Every level is written whole below, so none needs what the last frame left in it —
            // but the last frame's trace may still be sampling it, and the last frame's chain may
            // still be blitting it, so the discard waits for everything ahead of it on the queue.
            for (const Image* image : { tile.mSurface.get(), tile.mCurvature.get() })
                image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            const std::array<VkDescriptorBufferInfo, 3> blocks{
                VkDescriptorBufferInfo{ tile.mAmplitudes.getHandle(), 0, VK_WHOLE_SIZE },
                VkDescriptorBufferInfo{ tile.mFrequencies.getHandle(), 0, VK_WHOLE_SIZE },
                VkDescriptorBufferInfo{ tile.mField.getHandle(), 0, VK_WHOLE_SIZE },
            };
            const std::array<VkWriteDescriptorSet, 3> forms{
                bufferWrite(0, blocks[0]),
                bufferWrite(1, blocks[1]),
                bufferWrite(2, blocks[2]),
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
            vkCmdDispatch(commands, groupsFor(grid, sWaveWorkgroup), groupsFor(grid, sWaveWorkgroup), 1);
            handOver(commands);

            transform(commands, tile, grid);

            const std::array<VkDescriptorImageInfo, 2> images{
                VkDescriptorImageInfo{ VK_NULL_HANDLE, tile.mSurface->getStorageView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, tile.mCurvature->getStorageView(), VK_IMAGE_LAYOUT_GENERAL },
            };
            const std::array<VkWriteDescriptorSet, 3> composes{
                bufferWrite(0, blocks[2]),
                imageWrite(1, images[0]),
                imageWrite(2, images[1]),
            };
            const Shaders::WaveComposeConstants unpacked{ .mCount = grid };

            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mComposePipeline.getHandle());
            vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mComposePipeline.getLayout(), 0,
                static_cast<std::uint32_t>(composes.size()), composes.data());
            vkCmdPushConstants(
                commands, mComposePipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(unpacked), &unpacked);
            vkCmdDispatch(commands, groupsFor(grid, sWaveWorkgroup), groupsFor(grid, sWaveWorkgroup), 1);

            for (const Image* image : { tile.mSurface.get(), tile.mCurvature.get() })
                image->buildMips(commands);
        }
    }
}
