#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/shaders/probe.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/computepipeline.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/hostbuffer.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// The pattern in, the addresses of its blocks, and every reading out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> sBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
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

        /// `values` on the device, in whichever memory this leg of the test is asking about.
        ///
        /// Both of the kinds the renderer keeps geometry in: resizable-BAR video memory the host
        /// writes straight into, which is what the normals are, and ordinary device-local memory
        /// staged through a copy, which is what the indices and texture coordinates are.
        HostBuffer place(const Device& device, CommandPool&, std::span<const osg::Vec3f> values, HostBuffer*)
        {
            HostBuffer held(device, values.size_bytes(), sUsage);
            held.write(values);
            return held;
        }

        Buffer place(const Device& device, CommandPool& pool, std::span<const osg::Vec3f> values, Buffer*)
        {
            Batch upload(pool);
            Buffer held = uploadBuffer(device, upload, values, sUsage);
            upload.flush();
            return held;
        }

        /// Runs the probe and gives back the three readings end to end.
        std::vector<osg::Vec3f> runProbe(const Device& device, const ComputePipeline& pipeline, CommandPool& pool,
            VkBuffer source, VkDeviceAddress address, const HostBuffer& blocks)
        {
            const Buffer readings(device, sizeof(osg::Vec3f) * sCount * Shaders::PROBE_READINGS,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            const VkDescriptorBufferInfo from{ source, 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo into{ readings.getHandle(), 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo table{ blocks.getHandle(), 0, VK_WHOLE_SIZE };

            const auto write = [](std::uint32_t binding, const VkDescriptorBufferInfo& info) {
                return VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = binding,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &info,
                };
            };
            const std::array<VkWriteDescriptorSet, 3> writes{ write(0, from), write(1, into), write(2, table) };

            const Shaders::ProbeConstants constants{ .mSource = address, .mCount = sCount, .mBlock = sBlock };

            pool.submitAndWait([&](VkCommandBuffer commands) {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getHandle());
                vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0,
                    static_cast<std::uint32_t>(writes.size()), writes.data());
                vkCmdPushConstants(
                    commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
                vkCmdDispatch(commands, (sCount + Shaders::PROBE_WORKGROUP - 1) / Shaders::PROBE_WORKGROUP, 1, 1);

                const VkBufferMemoryBarrier2 written{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                    .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = readings.getHandle(),
                    .size = VK_WHOLE_SIZE,
                };
                const VkDependencyInfo dependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .bufferMemoryBarrierCount = 1,
                    .pBufferMemoryBarriers = &written,
                };
                vkCmdPipelineBarrier2(commands, &dependency);
            });

            std::vector<osg::Vec3f> read(sCount * Shaders::PROBE_READINGS);
            const void* mapped = readings.map();
            std::memcpy(read.data(), mapped, read.size() * sizeof(osg::Vec3f));
            readings.unmap();

            return read;
        }

        /// One memory kind, all three readings, against the pattern.
        template <class Held>
        void expectEveryReadingAgrees(const Device& device, const ComputePipeline& pipeline, CommandPool& pool,
            const std::vector<osg::Vec3f>& pattern, const std::string& memory)
        {
            const Held whole = place(device, pool, std::span<const osg::Vec3f>(pattern), static_cast<Held*>(nullptr));

            // The same pattern again, cut into separate buffers at separate addresses.
            std::vector<Held> blocks;
            std::vector<VkDeviceAddress> addresses;
            for (std::uint32_t start = 0; start < sCount; start += sBlock)
            {
                const std::span<const osg::Vec3f> part
                    = std::span<const osg::Vec3f>(pattern).subspan(start, std::min(sBlock, sCount - start));
                blocks.push_back(place(device, pool, part, static_cast<Held*>(nullptr)));
                addresses.push_back(blocks.back().getDeviceAddress());
            }

            ASSERT_EQ(addresses.size(), 3u) << "the block arithmetic is only exercised by more than one block";

            HostBuffer table(device, addresses.size() * sizeof(VkDeviceAddress), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            table.write(std::span<const VkDeviceAddress>(addresses));

            const std::vector<osg::Vec3f> read
                = runProbe(device, pipeline, pool, whole.getHandle(), whole.getDeviceAddress(), table);

            constexpr std::array<const char*, Shaders::PROBE_READINGS> sHow{
                "a descriptor",
                "a pointer the host handed over",
                "a pointer read out of the block table",
            };

            for (std::uint32_t reading = 0; reading < Shaders::PROBE_READINGS; ++reading)
                for (std::uint32_t at = 0; at < sCount; ++at)
                {
                    const osg::Vec3f& got = read[reading * sCount + at];
                    EXPECT_EQ(got, pattern[at])
                        << sHow[reading] << " read element " << at << " of " << memory << " memory as " << got.x()
                        << ", " << got.y() << ", " << got.z() << " rather than " << pattern[at].x() << ", "
                        << pattern[at].y() << ", " << pattern[at].z();
                }
        }

        /// A descriptor, a pointer, and a pointer out of a block table all read the same bytes.
        ///
        /// **The question that stopped the geometry blocking, asked of the device directly.** Moving
        /// the normal fetch from a descriptor to a pointer changed the picture on sixteen views and
        /// nothing else did; with nothing able to ask this, it had to be put to a whole traced frame
        /// and answered by elimination, which cost a day and reached the wrong answer twice.
        ///
        /// **Both memory kinds, because the renderer uses both.** Normals live in a `HostBuffer` —
        /// resizable-BAR video memory the host writes straight into, and write-combining — and
        /// indices and texture coordinates in ordinary device-local memory staged through a copy. A
        /// pointer read that only misbehaves in one of them would look like a shader bug.
        TEST(RtxProbeTest, aPointerAndADescriptorReadTheSameBytes)
        {
            std::string reason;
            const Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            const ComputePipeline pipeline(device, sBindings, sizeof(Shaders::ProbeConstants), {},
                Testing::getShaderDirectory() / "probe.comp.spv", "probe");
            CommandPool pool(device);

            const std::vector<osg::Vec3f> pattern = makePattern();

            expectEveryReadingAgrees<HostBuffer>(device, pipeline, pool, pattern, "host-visible");
            expectEveryReadingAgrees<Buffer>(device, pipeline, pool, pattern, "device-local");
        }
    }
}
