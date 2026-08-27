#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec2f>

#include <components/rtx/shaders/wave.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/computepipeline.hpp>
#include <components/rtxvulkan/device.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// The line the field is read and written through, and nothing else.
        constexpr std::array<VkDescriptorSetLayoutBinding, 1> sBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };

        /// A grid small enough to compare against a hand-written transform and large enough to run
        /// several stages of one — sixteen points is four.
        constexpr std::uint32_t sCount = 16;
        constexpr std::size_t sCells = std::size_t{ sCount } * sCount;

        /// The whole two-dimensional inverse transform, over a buffer the caller has filled.
        ///
        /// **Two dispatches of one shader.** A separable transform is the line transform run along
        /// the rows and then along the columns, which is the same code with its two strides swapped.
        std::vector<osg::Vec2f> transform(
            const Device& device, const ComputePipeline& pipeline, CommandPool& pool, std::span<const osg::Vec2f> grid)
        {
            const Buffer field(device, grid.size_bytes(),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            void* mapped = field.map();
            std::memcpy(mapped, grid.data(), grid.size_bytes());
            field.unmap();

            const VkDescriptorBufferInfo info{ field.getHandle(), 0, VK_WHOLE_SIZE };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &info,
            };

            pool.submitAndWait([&](VkCommandBuffer commands) {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getHandle());
                vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, 1, &write);

                for (int pass = 0; pass < 2; ++pass)
                {
                    // Rows first, where a line's points are one apart and the lines are a row apart,
                    // and then the columns with the two swapped.
                    const Shaders::WaveConstants constants{
                        .mCount = sCount,
                        .mStride = pass == 0 ? 1u : sCount,
                        .mJump = pass == 0 ? sCount : 1u,
                        .mOffset = 0,
                    };

                    vkCmdPushConstants(
                        commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
                    vkCmdDispatch(commands, sCount, 1, 1);

                    const VkMemoryBarrier2 between{
                        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT,
                    };
                    const VkDependencyInfo dependency{
                        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                        .memoryBarrierCount = 1,
                        .pMemoryBarriers = &between,
                    };
                    vkCmdPipelineBarrier2(commands, &dependency);
                }
            });

            std::vector<osg::Vec2f> read(grid.size());
            const void* out = field.map();
            std::memcpy(read.data(), out, grid.size_bytes());
            field.unmap();

            return read;
        }

        /// One wavevector transforms into the plane wave it stands for.
        ///
        /// **The whole of what a transform has to be right about, in one reading.** A single entry
        /// at `(column, row)` of a grid `n` across is the amplitude of `exp(i tau (c x + r y) / n)`,
        /// so the field it inverse-transforms to is that exponential sampled at every cell — and
        /// getting the sign, the stage order, the permutation on the way in or the twiddle's
        /// argument wrong all move it somewhere this comparison sees.
        ///
        /// Three entries rather than one, because a wavevector along an axis cannot tell the rows
        /// from the columns: the third leans both ways at once.
        TEST(RtxWaveLineTest, oneWavevectorTransformsIntoThePlaneWaveItStandsFor)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            CommandPool pool(device);
            const ComputePipeline pipeline(device, sBindings, sizeof(Shaders::WaveConstants), {},
                Testing::getShaderDirectory() / "waveline.comp.spv", "test-waveline");

            for (const std::pair<std::uint32_t, std::uint32_t> wavevector :
                { std::pair{ 1u, 0u }, std::pair{ 0u, 3u }, std::pair{ 2u, 5u } })
            {
                const auto [column, row] = wavevector;

                std::vector<osg::Vec2f> grid(sCells);
                grid[std::size_t{ row } * sCount + column] = osg::Vec2f(1.0f, 0.0f);

                const std::vector<osg::Vec2f> field = transform(device, pipeline, pool, grid);
                ASSERT_EQ(field.size(), sCells);

                for (std::uint32_t y = 0; y < sCount; ++y)
                    for (std::uint32_t x = 0; x < sCount; ++x)
                    {
                        const float angle = Shaders::TAU
                            * (static_cast<float>(column * x) + static_cast<float>(row * y))
                            / static_cast<float>(sCount);

                        const osg::Vec2f& got = field[std::size_t{ y } * sCount + x];

                        ASSERT_NEAR(got.x(), std::cos(angle), 1e-4f)
                            << "at " << x << ", " << y << " of wavevector " << column << ", " << row;
                        ASSERT_NEAR(got.y(), std::sin(angle), 1e-4f)
                            << "at " << x << ", " << y << " of wavevector " << column << ", " << row;
                    }
            }
        }

        /// A conjugate-symmetric grid transforms into a field with nothing imaginary left in it.
        ///
        /// **The property every real field the sea is made of depends on.** A spectrum whose entry
        /// at `-k` is the conjugate of the one at `k` has a real inverse, which is what lets two
        /// real fields share one transform: pack one into the real part of the spectrum and the
        /// other into the imaginary part, and the two come out separated.
        TEST(RtxWaveLineTest, aConjugateSymmetricGridTransformsIntoSomethingReal)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            CommandPool pool(device);
            const ComputePipeline pipeline(device, sBindings, sizeof(Shaders::WaveConstants), {},
                Testing::getShaderDirectory() / "waveline.comp.spv", "test-waveline");

            std::vector<osg::Vec2f> grid(sCells);

            // A handful of entries and their mirrors. Index arithmetic is modulo the grid, so the
            // partner of `(c, r)` sits at `(n - c, n - r)` with nought its own partner.
            for (const std::pair<std::uint32_t, std::uint32_t> at :
                { std::pair{ 1u, 2u }, std::pair{ 5u, 0u }, std::pair{ 3u, 7u } })
            {
                const auto [column, row] = at;
                const osg::Vec2f value(0.25f * static_cast<float>(column), -0.125f * static_cast<float>(row) - 0.5f);

                grid[std::size_t{ row } * sCount + column] = value;
                grid[std::size_t{ (sCount - row) % sCount } * sCount + (sCount - column) % sCount]
                    = osg::Vec2f(value.x(), -value.y());
            }

            const std::vector<osg::Vec2f> field = transform(device, pipeline, pool, grid);

            float largest = 0.0f;
            for (const osg::Vec2f& value : field)
                largest = std::max(largest, std::abs(value.y()));

            EXPECT_LT(largest, 1e-4f) << "the imaginary part of a real field";
        }
    }
}
