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

#include <components/rtx/shaders/gbuffer.h>
#include <components/rtx/shaders/wave.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/computepipeline.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/image.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// Small enough to compare every texel against a hand-written wave, and large enough that
        /// the transform runs four of its stages.
        constexpr std::uint32_t sCount = 16;
        constexpr std::size_t sCells = std::size_t{ sCount } * sCount;

        /// How wide the tile is. Round, so a texel is a whole number of units and the expectations
        /// below are exact rather than nearly so.
        constexpr float sExtent = 256.0f;

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

        /// What one texel of the surface and the curvature came out as.
        struct Sampled
        {
            osg::Vec2f mSlope;
            float mSlopeSquared;
            float mHeightSquared;

            osg::Vec3f mCurve;
        };

        /// The three the chain runs through, built once for a whole test.
        struct Passes
        {
            ComputePipeline mForming;
            ComputePipeline mLine;
            ComputePipeline mComposing;

            explicit Passes(const Device& device)
                : mForming(device, sFormBindings, sizeof(Shaders::WaveFormConstants), {},
                    Testing::getShaderDirectory() / "waveform.comp.spv", "test-waveform")
                , mLine(device, sLineBindings, sizeof(Shaders::WaveConstants), {},
                      Testing::getShaderDirectory() / "waveline.comp.spv", "test-waveline")
                , mComposing(device, sComposeBindings, sizeof(Shaders::WaveComposeConstants), {},
                      Testing::getShaderDirectory() / "wavecompose.comp.spv", "test-wavecompose")
            {
            }
        };

        /// Runs the whole chain — form, transform along both axes, compose — over one spectrum.
        std::vector<Sampled> run(const Device& device, CommandPool& pool, const Passes& passes,
            std::span<const osg::Vec2f> amplitudes, std::span<const float> frequencies, float time)
        {
            const ComputePipeline& forming = passes.mForming;
            const ComputePipeline& line = passes.mLine;
            const ComputePipeline& composing = passes.mComposing;

            const Buffer table = Buffer::staging(device, amplitudes.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            const Buffer turning
                = Buffer::staging(device, frequencies.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            const Buffer field
                = Buffer::staging(device, 3 * sCells * sizeof(osg::Vec2f), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

            std::memcpy(table.map(), amplitudes.data(), amplitudes.size_bytes());
            std::memcpy(turning.map(), frequencies.data(), frequencies.size_bytes());

            constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            const Image surface(device, sCount, sCount, GBUFFER_ALBEDO, usage, "test-wave-surface");
            const Image curvature(device, sCount, sCount, GBUFFER_ALBEDO, usage, "test-wave-curvature");

            const auto buffer = [](std::uint32_t binding, const VkDescriptorBufferInfo& info) {
                return VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = binding,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &info,
                };
            };
            const auto stored = [](std::uint32_t binding, const VkDescriptorImageInfo& info) {
                return VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = binding,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &info,
                };
            };

            const VkDescriptorBufferInfo whole[]{ { table.getHandle(), 0, VK_WHOLE_SIZE },
                { turning.getHandle(), 0, VK_WHOLE_SIZE }, { field.getHandle(), 0, VK_WHOLE_SIZE } };

            const VkDescriptorImageInfo images[]{ { VK_NULL_HANDLE, surface.getView(), VK_IMAGE_LAYOUT_GENERAL },
                { VK_NULL_HANDLE, curvature.getView(), VK_IMAGE_LAYOUT_GENERAL } };

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

            pool.submitAndWait([&](VkCommandBuffer commands) {
                for (const Image* image : { &surface, &curvature })
                    image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

                const std::array<VkWriteDescriptorSet, 3> forms{ buffer(0, whole[0]), buffer(1, whole[1]),
                    buffer(2, whole[2]) };
                const Shaders::WaveFormConstants shaped{ .mCount = sCount, .mExtent = sExtent, .mTime = time };

                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, forming.getHandle());
                vkCmdPushDescriptorSet(
                    commands, VK_PIPELINE_BIND_POINT_COMPUTE, forming.getLayout(), 0, 3, forms.data());
                vkCmdPushConstants(
                    commands, forming.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shaped), &shaped);
                vkCmdDispatch(commands, (sCount + 7) / 8, (sCount + 7) / 8, 1);
                vkCmdPipelineBarrier2(commands, &dependency);

                const std::array<VkWriteDescriptorSet, 1> lines{ buffer(0, whole[2]) };
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, line.getHandle());
                vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, line.getLayout(), 0, 1, lines.data());

                for (std::uint32_t pair = 0; pair < 3; ++pair)
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        const Shaders::WaveConstants along{
                            .mCount = sCount,
                            .mStride = pass == 0 ? 1u : sCount,
                            .mJump = pass == 0 ? sCount : 1u,
                            .mOffset = pair * static_cast<std::uint32_t>(sCells),
                        };

                        vkCmdPushConstants(
                            commands, line.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(along), &along);
                        vkCmdDispatch(commands, sCount, 1, 1);
                        vkCmdPipelineBarrier2(commands, &dependency);
                    }

                const std::array<VkWriteDescriptorSet, 3> composes{ buffer(0, whole[2]), stored(1, images[0]),
                    stored(2, images[1]) };
                const Shaders::WaveComposeConstants unpacked{ .mCount = sCount };

                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, composing.getHandle());
                vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, composing.getLayout(), 0,
                    static_cast<std::uint32_t>(composes.size()), composes.data());
                vkCmdPushConstants(
                    commands, composing.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(unpacked), &unpacked);
                vkCmdDispatch(commands, (sCount + 7) / 8, (sCount + 7) / 8, 1);
            });

            const std::vector<float> heights = Testing::readHalves(pool, surface);
            const std::vector<float> curves = Testing::readHalves(pool, curvature);

            std::vector<Sampled> read(sCells);
            for (std::size_t at = 0; at < sCells; ++at)
                read[at] = Sampled{
                    .mSlope = osg::Vec2f(heights[at * 4], heights[at * 4 + 1]),
                    .mSlopeSquared = heights[at * 4 + 2],
                    .mHeightSquared = heights[at * 4 + 3],
                    .mCurve = osg::Vec3f(curves[at * 4], curves[at * 4 + 1], curves[at * 4 + 2]),
                };

            return read;
        }

        struct RtxWaveFieldTest : Testing::DeviceTest
        {
        };

        /// One amplitude becomes the cosine it stands for, and its own two derivatives beside it.
        ///
        /// **Everything between the host's spectrum and the shader's surface, in one reading.** A
        /// single entry with no partner is still a real field, because the pass adds the conjugate
        /// at `-k` itself — so a lone `A` at wavevector `k` is the wave
        ///
        ///     h(x) = 2 A cos(k . x + w t)
        ///
        /// and the slope and the curvature are that differentiated once and twice. Getting the
        /// mirror index, the sign of the turn, the `i k` factors, the packing of two fields into one
        /// transform, or the half-grid shift wrong all move this somewhere the comparison sees.
        TEST_F(RtxWaveFieldTest, oneAmplitudeBecomesTheWaveItStandsForAndItsDerivatives)
        {
            const Device& device = getDevice();
            CommandPool& pool = getPool();
            const Passes passes(device);

            constexpr float amplitude = 0.5f;
            constexpr int middle = static_cast<int>(sCount) / 2;

            for (const std::pair<int, int> place : { std::pair{ 3, 0 }, std::pair{ 0, -2 }, std::pair{ 2, -5 } })
            {
                const auto [alongX, alongY] = place;

                std::vector<osg::Vec2f> table(sCells);
                std::vector<float> turning(sCells, 0.0f);

                const std::size_t at
                    = static_cast<std::size_t>(alongY + middle) * sCount + static_cast<std::size_t>(alongX + middle);
                table[at] = osg::Vec2f(amplitude, 0.0f);

                // Nought, so the wave stands still and the expectation carries no phase of its own.
                // What the frequency does is tested where it comes from.
                const std::vector<Sampled> field = run(device, pool, passes, table, turning, 0.0f);
                ASSERT_EQ(field.size(), sCells);

                const float step = Shaders::TAU / sExtent;
                const osg::Vec2f wavevector(step * static_cast<float>(alongX), step * static_cast<float>(alongY));
                const float texel = sExtent / static_cast<float>(sCount);

                for (std::uint32_t y = 0; y < sCount; ++y)
                    for (std::uint32_t x = 0; x < sCount; ++x)
                    {
                        const float phase
                            = texel * (wavevector.x() * static_cast<float>(x) + wavevector.y() * static_cast<float>(y));

                        const float wave = 2.0f * amplitude * std::cos(phase);
                        const float derivative = -2.0f * amplitude * std::sin(phase);

                        const Sampled& got = field[std::size_t{ y } * sCount + x];
                        const std::string where = " at " + std::to_string(x) + ", " + std::to_string(y) + " of "
                            + std::to_string(alongX) + ", " + std::to_string(alongY);

                        ASSERT_NEAR(got.mSlope.x(), wavevector.x() * derivative, 5e-3f) << "slope x" << where;
                        ASSERT_NEAR(got.mSlope.y(), wavevector.y() * derivative, 5e-3f) << "slope y" << where;

                        ASSERT_NEAR(got.mCurve.x(), -wavevector.x() * wavevector.x() * wave, 5e-3f)
                            << "curvature xx" << where;
                        ASSERT_NEAR(got.mCurve.y(), -wavevector.y() * wavevector.y() * wave, 5e-3f)
                            << "curvature yy" << where;
                        ASSERT_NEAR(got.mCurve.z(), -wavevector.x() * wavevector.y() * wave, 5e-3f)
                            << "curvature xy" << where;

                        // The two moments a mip chain is asked for, which have to be the squares of
                        // what sits beside them rather than anything of their own. **And the second
                        // is the whole of what pins the elevation**, which the pass no longer stores
                        // on its own: nothing shades from it, and the slope and the curvature carry
                        // its sign between them.
                        ASSERT_NEAR(got.mHeightSquared, wave * wave, 5e-3f) << "height squared" << where;
                        ASSERT_NEAR(got.mSlopeSquared, got.mSlope * got.mSlope, 5e-3f) << "slope squared" << where;
                    }
            }
        }
    }
}
