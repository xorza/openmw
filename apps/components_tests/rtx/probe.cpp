#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/rtx/shaders/probe.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/computepipeline.hpp>
#include <components/rtxvulkan/device.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// The pattern in, every reading out, the addresses of its blocks, the two addresses a
        /// uniform block carries, and the rows out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 5> sBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };

        /// Enough to cross the workgroup several times and end partway through one: 300 is four
        /// full groups of 64 and a tail of 44.
        constexpr std::uint32_t sCount = 300;

        /// Elements per block, so the table below holds three of them and the last is a part block —
        /// 128, 128 and 44. Small on purpose: `VERTEX_BLOCK` is 256 Ki and every scene this fork has
        /// rendered fits in one, which is exactly why the block index has never been exercised.
        constexpr std::uint32_t sBlock = 128;

        constexpr VkBufferUsageFlags sUsage
            = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        /// What the device is asked to read back.
        ///
        /// Every value is a sum of powers of two, so it survives the trip exactly and an equality
        /// comparison is a statement about the hardware rather than about rounding. The three
        /// channels are pulled apart — different magnitudes, one of them negative — so a reading
        /// that swapped or shifted components cannot pass.
        std::vector<osg::Vec3f> makePattern()
        {
            std::vector<osg::Vec3f> pattern;
            pattern.reserve(sCount);
            for (std::uint32_t at = 0; at < sCount; ++at)
            {
                const auto index = static_cast<float>(at);
                pattern.emplace_back(index * 0.25f + 1.0f, -index * 0.5f - 2.0f, index * 0.125f + 0.375f);
            }

            return pattern;
        }

        /// The same idea over rows: twelve channels a row, every one of them exact, no two alike.
        std::vector<Shaders::ProbeRow> makeRows()
        {
            std::vector<Shaders::ProbeRow> rows;
            rows.reserve(sCount);
            for (std::uint32_t at = 0; at < sCount; ++at)
            {
                const auto index = static_cast<float>(at);
                rows.push_back(Shaders::ProbeRow{
                    .mA = osg::Vec4f(index * 0.5f + 1.0f, -index * 0.25f - 2.0f, index * 0.125f + 0.375f, index),
                    .mB = osg::Vec4f(index + 1024.0f, -index * 2.0f, index * 0.0625f, -index - 0.5f),
                    .mC = osg::Vec4f(index * 4.0f + 3.0f, index * 0.5f - 1.0f, -index * 0.125f, index + 0.75f),
                });
            }

            return rows;
        }

        /// `bytes` in resizable-BAR video memory, which is where the normals are.
        Buffer placeHostWritten(const Device& device, CommandPool&, std::span<const std::byte> bytes)
        {
            Buffer held = Buffer::hostWritten(device, bytes.size(), sUsage);
            held.write(bytes);
            return held;
        }

        /// `bytes` in ordinary device-local memory staged through a copy, which is where the
        /// indices and the texture coordinates are.
        Buffer placeStaged(const Device& device, CommandPool& pool, std::span<const std::byte> bytes)
        {
            Batch upload(pool);
            Buffer held = uploadBuffer(device, upload, bytes, sUsage);
            upload.flush();
            return held;
        }

        /// Which of the two a leg of the test asks about.
        using Place = Buffer (*)(const Device&, CommandPool&, std::span<const std::byte>);

        /// Everything the probe wrote: the four readings of the pattern end to end, and the rows.
        struct Readings
        {
            std::vector<osg::Vec3f> mValues;
            std::vector<Shaders::ProbeRow> mRows;
        };

        /// Runs the probe and gives back everything it read.
        ///
        /// @param addresses the uniform block holding the pattern's address and the rows'.
        Readings runProbe(const Device& device, const ComputePipeline& pipeline, CommandPool& pool, VkBuffer source,
            VkDeviceAddress address, const Buffer& blocks, const Buffer& addresses)
        {
            const Buffer readings = Buffer::staging(
                device, sizeof(osg::Vec3f) * sCount * Shaders::PROBE_READINGS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            const Buffer rowReadings
                = Buffer::staging(device, sizeof(Shaders::ProbeRow) * sCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

            const VkDescriptorBufferInfo from{ source, 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo into{ readings.getHandle(), 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo table{ blocks.getHandle(), 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo addressed{ addresses.getHandle(), 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo rowsInto{ rowReadings.getHandle(), 0, VK_WHOLE_SIZE };

            const auto write = [](std::uint32_t binding, const VkDescriptorBufferInfo& info) {
                return VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = binding,
                    .descriptorCount = 1,
                    .descriptorType = sBindings[binding].descriptorType,
                    .pBufferInfo = &info,
                };
            };
            const std::array<VkWriteDescriptorSet, sBindings.size()> writes{ write(0, from), write(1, into),
                write(2, table), write(3, addressed), write(4, rowsInto) };

            const Shaders::ProbeConstants constants{ .mSource = address, .mCount = sCount, .mBlock = sBlock };

            pool.submitAndWait([&](VkCommandBuffer commands) {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getHandle());
                vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0,
                    static_cast<std::uint32_t>(writes.size()), writes.data());
                vkCmdPushConstants(
                    commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
                vkCmdDispatch(commands, (sCount + Shaders::PROBE_WORKGROUP - 1) / Shaders::PROBE_WORKGROUP, 1, 1);

                const auto written = [](const Buffer& buffer) {
                    return VkBufferMemoryBarrier2{
                        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .buffer = buffer.getHandle(),
                        .size = VK_WHOLE_SIZE,
                    };
                };
                const std::array<VkBufferMemoryBarrier2, 2> barriers{ written(readings), written(rowReadings) };
                const VkDependencyInfo dependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .bufferMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()),
                    .pBufferMemoryBarriers = barriers.data(),
                };
                vkCmdPipelineBarrier2(commands, &dependency);
            });

            Readings read;
            read.mValues.resize(sCount * Shaders::PROBE_READINGS);
            std::memcpy(read.mValues.data(), readings.map(), read.mValues.size() * sizeof(osg::Vec3f));
            read.mRows.resize(sCount);
            std::memcpy(read.mRows.data(), rowReadings.map(), read.mRows.size() * sizeof(Shaders::ProbeRow));

            return read;
        }

        std::string describe(const osg::Vec4f& value)
        {
            return std::to_string(value.x()) + ", " + std::to_string(value.y()) + ", " + std::to_string(value.z())
                + ", " + std::to_string(value.w());
        }

        /// One memory kind, every reading, against the patterns.
        void expectEveryReadingAgrees(const Device& device, const ComputePipeline& pipeline, CommandPool& pool,
            const std::vector<osg::Vec3f>& pattern, const std::vector<Shaders::ProbeRow>& rows, Place place,
            const std::string& memory)
        {
            const Buffer whole = place(device, pool, std::as_bytes(std::span<const osg::Vec3f>(pattern)));

            // The same pattern again, cut into separate buffers at separate addresses.
            std::vector<Buffer> blocks;
            std::vector<VkDeviceAddress> addresses;
            for (std::uint32_t start = 0; start < sCount; start += sBlock)
            {
                const std::span<const osg::Vec3f> part
                    = std::span<const osg::Vec3f>(pattern).subspan(start, std::min(sBlock, sCount - start));
                blocks.push_back(place(device, pool, std::as_bytes(part)));
                addresses.push_back(blocks.back().getDeviceAddress());
            }

            ASSERT_EQ(addresses.size(), 3u) << "the block arithmetic is only exercised by more than one block";

            Buffer table = Buffer::hostWritten(
                device, addresses.size() * sizeof(VkDeviceAddress), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            table.write(std::span<const VkDeviceAddress>(addresses));

            // The rows in the same memory as the pattern, and both addresses in a uniform block.
            const Buffer held = place(device, pool, std::as_bytes(std::span<const Shaders::ProbeRow>(rows)));

            const Shaders::ProbeAddresses named{ .mSource = whole.getDeviceAddress(),
                .mRows = held.getDeviceAddress() };
            ASSERT_EQ(named.mRows % Shaders::PROBE_ROW_ALIGN, 0u)
                << "the rows' reference claims an alignment the buffer does not have";

            Buffer uniform = Buffer::hostWritten(device, sizeof(named), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            uniform.write(std::span<const Shaders::ProbeAddresses>(&named, 1));

            const Readings read = runProbe(device, pipeline, pool, whole.getHandle(), named.mSource, table, uniform);

            constexpr std::array<const char*, Shaders::PROBE_READINGS> sHow{
                "a descriptor",
                "a pointer the host handed over",
                "a pointer read out of the block table",
                "a pointer read out of a uniform block",
            };

            for (std::uint32_t reading = 0; reading < Shaders::PROBE_READINGS; ++reading)
                for (std::uint32_t at = 0; at < sCount; ++at)
                {
                    const osg::Vec3f& got = read.mValues[reading * sCount + at];
                    EXPECT_EQ(got, pattern[at])
                        << sHow[reading] << " read element " << at << " of " << memory << " memory as " << got.x()
                        << ", " << got.y() << ", " << got.z() << " rather than " << pattern[at].x() << ", "
                        << pattern[at].y() << ", " << pattern[at].z();
                }

            for (std::uint32_t at = 0; at < sCount; ++at)
            {
                const Shaders::ProbeRow& got = read.mRows[at];
                const Shaders::ProbeRow& want = rows[at];
                EXPECT_TRUE(got.mA == want.mA && got.mB == want.mB && got.mC == want.mC)
                    << "a sixteen-aligned reference read row " << at << " of " << memory << " memory as "
                    << describe(got.mA) << " / " << describe(got.mB) << " / " << describe(got.mC) << " rather than "
                    << describe(want.mA) << " / " << describe(want.mB) << " / " << describe(want.mC);
            }
        }

        struct RtxProbeTest : Testing::DeviceTest
        {
        };

        /// A descriptor, a pointer, a pointer out of a block table and a pointer out of a uniform
        /// block all read the same bytes — and a reference that claims sixteen-byte alignment reads
        /// a `GpuLayer`-shaped row whole.
        ///
        /// **The question that stopped the geometry blocking, asked of the device directly.** Moving
        /// the normal fetch from a descriptor to a pointer changed the picture on sixteen views and
        /// nothing else did; with nothing able to ask this, it had to be put to a whole traced frame
        /// and answered by elimination, which cost a day and reached the wrong answer twice.
        ///
        /// **Both memory kinds, because the renderer uses both.** Normals live in resizable-BAR
        /// video memory the host writes straight into, which is write-combining, and indices and
        /// texture coordinates in ordinary device-local memory staged through a copy. A pointer read
        /// that only misbehaves in one of them would look like a shader bug.
        TEST_F(RtxProbeTest, aPointerAndADescriptorReadTheSameBytes)
        {
            const Device& device = getDevice();
            const ComputePipeline pipeline(device, sBindings, sizeof(Shaders::ProbeConstants), {},
                Testing::getShaderDirectory() / "probe.comp.spv", "probe");
            CommandPool pool(device);

            const std::vector<osg::Vec3f> pattern = makePattern();
            const std::vector<Shaders::ProbeRow> rows = makeRows();

            expectEveryReadingAgrees(device, pipeline, pool, pattern, rows, placeHostWritten, "host-visible");
            expectEveryReadingAgrees(device, pipeline, pool, pattern, rows, placeStaged, "device-local");
        }
    }
}
