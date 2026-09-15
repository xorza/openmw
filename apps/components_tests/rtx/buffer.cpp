#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/graveyard.hpp>
#include <components/rtxvulkan/timeline.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxBufferTest : Testing::DeviceTest
        {
        };

        /// A buffer the host can reach is mapped once and keeps the address for its life.
        ///
        /// **What a frame pays for asking twice.** `vkMapMemory` takes a lock inside the driver and
        /// hands back an address that never moves, so a buffer the host rewrites every frame — the
        /// hit count is one — was paying a pair of driver calls a frame for a pointer it held.
        TEST_F(RtxBufferTest, aHostVisibleBufferIsMappedOnceAndKeepsTheAddress)
        {
            const Device& device = *mHarness->mDevice;

            const Buffer buffer = Buffer::staging(device, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "test");

            void* const mapped = buffer.map();
            ASSERT_NE(mapped, nullptr);
            EXPECT_EQ(buffer.map(), mapped) << "a second map moved the buffer";

            // And a write lands there, with nothing said to make it visible: the memory is coherent
            // and the mapping is the same one.
            const std::array<std::uint32_t, 2> written{ 7, 9 };
            buffer.write(std::span<const std::uint32_t>(written));

            const auto* read = static_cast<const std::uint32_t*>(mapped);
            EXPECT_EQ(read[0], 7u);
            EXPECT_EQ(read[1], 9u);
        }

        /// A buffer moved out of takes its mapping with it, and the husk has none.
        ///
        /// **Which is what a table growing does**: `growTo` hands the displaced buffer to a
        /// graveyard, and the new one is moved into the member the old one was in.
        TEST_F(RtxBufferTest, aMovedBufferTakesItsMappingWithIt)
        {
            const Device& device = *mHarness->mDevice;

            Buffer first = Buffer::staging(device, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "test");

            void* const mapped = first.map();
            const Buffer second = std::move(first);

            EXPECT_EQ(second.map(), mapped) << "the mapping did not come across";
        }

        /// A copy names both of its ends for the submit it rides, and a host write of either waits
        /// for that submit and no longer.
        ///
        /// **What `isIdle` was not told.** A hand-out by address or by descriptor named its buffer,
        /// and a copy — which takes handles — named nothing, so a table written on the queue and
        /// then from the host was two writers in an order nobody had fixed, and the assert that
        /// guards a host write had nothing to fire on. The hold is what makes the queue's side of
        /// this a state rather than a race: the copy is on the queue for as long as the test says.
        TEST_F(RtxBufferTest, aCopyNamesBothEndsAndAHostWriteWaitsForIt)
        {
            const Device& device = *mHarness->mDevice;
            CommandPool& pool = getPool();
            Graveyard graveyard(device, pool);

            constexpr VkBufferUsageFlags copyable = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            const Buffer source = Buffer::staging(device, 64, copyable, "test");
            const Buffer target = Buffer::staging(device, 64, copyable, "test");

            std::array<std::uint32_t, 16> counted{};
            for (std::size_t at = 0; at < counted.size(); ++at)
                counted[at] = static_cast<std::uint32_t>(at + 1);
            source.write(std::span<const std::uint32_t>(counted));

            EXPECT_TRUE(source.isIdle()) << "a buffer nothing has named";
            EXPECT_TRUE(target.isIdle());

            Testing::HeldSubmit hold(device);
            const VkCommandBuffer commands = pool.allocate(1).front();
            pool.begin(commands);
            source.copyTo(commands, target, 64);

            const std::uint64_t next = device.getTimeline().getNext();
            EXPECT_EQ(source.getNamedUntil(), next) << "the copy's source was not named";
            EXPECT_EQ(target.getNamedUntil(), next) << "the copy's destination was not named";
            EXPECT_TRUE(target.isIdle()) << "named for a submit nobody has made, which a host write lands ahead of";

            EXPECT_EQ(hold.submit(pool, commands, graveyard), next);
            EXPECT_FALSE(source.isIdle()) << "the copy is on the queue";
            EXPECT_FALSE(target.isIdle()) << "the copy is on the queue";

            hold.release();
            target.waitIdle("test");
            EXPECT_TRUE(target.isIdle());
            EXPECT_TRUE(source.isIdle()) << "one submit carried both ends";

            const auto* landed = static_cast<const std::uint32_t*>(target.map());
            for (std::size_t at = 0; at < counted.size(); ++at)
                EXPECT_EQ(landed[at], counted[at]) << at;

            // And idle is a state a wait need not leave for: nothing names the buffer now.
            target.waitIdle("test");
            EXPECT_TRUE(target.isIdle());
        }
    }
}
