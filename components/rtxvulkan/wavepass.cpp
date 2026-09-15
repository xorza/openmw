#include "wavepass.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>

#include <osg/Vec2f>

#include "barriers.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "dispatch.hpp"

namespace Rtx
{
    namespace
    {
        /// The amplitudes, how fast each turns, and the three packed fields between them.
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> sFormBindings
            = computeBindings<3>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        constexpr std::array<VkDescriptorSetLayoutBinding, 1> sLineBindings
            = computeBindings<1>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        /// The fields in, and the two textures out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> sComposeBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };

        /// How many complex numbers the transform runs over for one tile: three packed fields, each
        /// the grid itself.
        std::size_t fieldOf(std::size_t grid)
        {
            return 3 * grid * grid;
        }

        /// What a capture calls one of a cascade's objects, or nothing where no build names any:
        /// the formatting is a trip to the heap for a name that goes nowhere.
        std::string tileName([[maybe_unused]] std::string_view what, [[maybe_unused]] std::size_t cascade)
        {
            if constexpr (Device::wantsNames())
                return std::format("wave {} {}", what, cascade);
            else
                return {};
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
        , mSampler(makeContentSampler(device, "wave"))
    {
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            Tile& tile = mTiles[index];
            const std::uint32_t grid = static_cast<std::uint32_t>(sWaveTiles[index].mGrid);
            const std::uint32_t levels = levelsFor(sWaveTiles[index].mGrid);

            tile.mField = Buffer::deviceLocal(mDevice, fieldOf(sWaveTiles[index].mGrid) * 2 * sizeof(float),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tileName("field", index));

            tile.mSurface = Image(mDevice, grid, grid, WAVE_TILE_FORMAT, usage, tileName("surface", index), levels);
            tile.mCurvature = Image(mDevice, grid, grid, WAVE_TILE_FORMAT, usage, tileName("curvature", index), levels);
        }

        describe(mSea);

        // Every tile in the layout the trace binds it in, from the first frame: a frame with no
        // water synthesises nothing and binds the tiles anyway, and a descriptor naming an image
        // that was never transitioned is an error whether or not a ray samples it.
        mPool.submitAndWait([&](VkCommandBuffer commands) { record(commands, 0.0f); });
    }

    void WavePass::describe(const SeaState& sea)
    {
        const std::array<WaveCascade, Shaders::WAVE_CASCADES> cascades = makeWaveCascades(sea);

        mSlope = waveSlope(cascades);
        mCurvature = waveCurvature(cascades);

        Batch batch(mPool);
        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            mTiles[index].mAmplitudes = uploadBuffer(batch, std::span<const osg::Vec2f>(cascades[index].mAmplitudes),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tileName("amplitudes", index));
            mTiles[index].mFrequencies = uploadBuffer(batch, std::span<const float>(cascades[index].mFrequencies),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tileName("frequencies", index));
        }
        batch.flush();

        mSea = sea;
    }

    void WavePass::handOver(VkCommandBuffer commands) const
    {
        // The synthesis is a dispatch and the trace that samples what it left is a launch. Written
        // as well as read, because the transform runs in place and a dependency naming only the
        // read leaves the two writes unordered.
        Rtx::handOver(commands, Use::sBufferComputeWrite,
            BufferUse{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT });
    }

    void WavePass::transform(VkCommandBuffer commands, const Tile& tile, std::uint32_t count) const
    {
        // Bound and pushed once for the six dispatches below, which differ in their constants alone.
        DescriptorWrites<1> writes;
        writes.buffer(0, tile.mField.describe());

        bind(commands, mLinePipeline);
        pushDescriptors(commands, mLinePipeline, writes.get());

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

                pushConstants(commands, mLinePipeline, along);
                vkCmdDispatch(commands, count, 1, 1);
                handOver(commands);
            }
    }

    void WavePass::record(VkCommandBuffer commands, float seconds) const
    {
        for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
        {
            const Tile& tile = mTiles[index];
            const std::uint32_t grid = static_cast<std::uint32_t>(sWaveTiles[index].mGrid);

            // Every level is written whole below, so none needs what the last frame left in it.
            // The last frame's trace may still be sampling it, and the head barrier
            // `CommandPool::begin` recorded is what orders this buffer after that.
            Barriers opened(commands);
            for (const Image* image : { &tile.mSurface, &tile.mCurvature })
                opened.add(image->describeTransition(Use::sUndefined, Use::sComputeWrite));

            opened.flush();

            DescriptorWrites<3> forms;
            forms.buffer(0, tile.mAmplitudes.describe());
            forms.buffer(1, tile.mFrequencies.describe());
            forms.buffer(2, tile.mField.describe());

            const Shaders::WaveFormConstants shaped{
                .mCount = grid,
                .mExtent = sWaveTiles[index].mExtent,
                .mTime = seconds,
            };
            dispatch(commands, mFormPipeline, forms.get(), shaped, groupsFor(grid, Shaders::WAVE_TILE_WORKGROUP),
                groupsFor(grid, Shaders::WAVE_TILE_WORKGROUP));
            handOver(commands);

            transform(commands, tile, grid);

            DescriptorWrites<3> composes;
            composes.buffer(0, tile.mField.describe());
            composes.image(1, tile.mSurface.describeStorage());
            composes.image(2, tile.mCurvature.describeStorage());

            const Shaders::WaveComposeConstants unpacked{ .mCount = grid };
            dispatch(commands, mComposePipeline, composes.get(), unpacked,
                groupsFor(grid, Shaders::WAVE_TILE_WORKGROUP), groupsFor(grid, Shaders::WAVE_TILE_WORKGROUP));

            for (const Image* image : { &tile.mSurface, &tile.mCurvature })
                image->buildMips(commands);
        }
    }
}
