#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/tracepipeline.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// Where each invocation writes what its launch index was.
        constexpr std::array<VkDescriptorSetLayoutBinding, 1> sBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR },
        };

        /// A grid that is neither square nor a multiple of a warp, so a launch that rounded its
        /// extent up or laid its rows out at the wrong stride is a failure rather than a coincidence.
        constexpr std::uint32_t sWidth = 37;
        constexpr std::uint32_t sHeight = 11;

        /// One launch index, as the shader writes it.
        struct Launched
        {
            std::uint32_t mX;
            std::uint32_t mY;
        };

        struct RtxTracePipelineTest : Testing::DeviceTest
        {
        };

        /// A launch runs once at every index of the grid it was given.
        ///
        /// **The shader binding table and `vkCmdTraceRaysKHR`, asked of the device directly.** The
        /// trace rests on both and neither says anything when it is wrong: a table whose one handle
        /// landed at the wrong offset, or a launch of the wrong grid, is a picture that is missing
        /// or garbled rather than an error anything reports. Every slot is filled with a value that
        /// cannot occur, so an invocation that never ran and one that ran twice are different
        /// failures.
        TEST_F(RtxTracePipelineTest, aLaunchRunsOnceAtEveryIndex)
        {
            const Device& device = getDevice();

            // The one record the probe's own hit object names. `traceprobe.rmiss` says what a table
            // without it costs.
            const std::array<std::filesystem::path, 1> miss{ Testing::getShaderDirectory() / "traceprobe.rmiss.spv" };
            const TraceShaders shaders{
                .mRaygen = Testing::getShaderDirectory() / "traceprobe.rgen.spv",
                .mMiss = miss,
            };
            const TracePipeline pipeline(device, sBindings, {}, shaders, "trace probe");

            constexpr std::uint32_t sCount = sWidth * sHeight;
            const Buffer written
                = Buffer::staging(device, sCount * sizeof(Launched), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

            constexpr std::uint32_t sUnwritten = 0xFFFFFFFFu;
            std::memset(written.map(), 0xFF, sCount * sizeof(Launched));

            const VkDescriptorBufferInfo into{ written.getHandle(), 0, VK_WHOLE_SIZE };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &into,
            };

            getPool().submitAndWait([&](VkCommandBuffer commands) {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline.getHandle());
                vkCmdPushDescriptorSet(
                    commands, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline.getLayout(), 0, 1, &write);
                pipeline.traceRays(commands, sWidth, sHeight);

                const VkBufferMemoryBarrier2 done{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                    .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = written.getHandle(),
                    .size = VK_WHOLE_SIZE,
                };
                const VkDependencyInfo dependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .bufferMemoryBarrierCount = 1,
                    .pBufferMemoryBarriers = &done,
                };
                vkCmdPipelineBarrier2(commands, &dependency);
            });

            std::vector<Launched> read(sCount);
            std::memcpy(read.data(), written.map(), read.size() * sizeof(Launched));

            for (std::uint32_t y = 0; y < sHeight; ++y)
                for (std::uint32_t x = 0; x < sWidth; ++x)
                {
                    const Launched& at = read[y * sWidth + x];
                    if (at.mX == sUnwritten && at.mY == sUnwritten)
                    {
                        ADD_FAILURE() << "no invocation wrote " << x << ", " << y;
                        continue;
                    }

                    EXPECT_EQ(at.mX, x) << "the invocation at " << x << ", " << y << " reported column " << at.mX;
                    EXPECT_EQ(at.mY, y) << "the invocation at " << x << ", " << y << " reported row " << at.mY;
                }
        }
    }
}
