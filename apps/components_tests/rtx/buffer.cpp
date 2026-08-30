#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/device.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        class RtxBufferTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                std::string reason;
                mHarness = Testing::getHarness(reason);
                if (mHarness == nullptr)
                    GTEST_SKIP() << reason;
            }

            Testing::Harness* mHarness = nullptr;
        };

        /// A buffer the host can reach is mapped once and keeps the address for its life.
        ///
        /// **What a frame pays for asking twice.** `vkMapMemory` takes a lock inside the driver and
        /// hands back an address that never moves, so a buffer the host rewrites every frame — the
        /// hit count is one — was paying a pair of driver calls a frame for a pointer it held.
        TEST_F(RtxBufferTest, aHostVisibleBufferIsMappedOnceAndKeepsTheAddress)
        {
            const Device& device = *mHarness->mDevice;

            const Buffer buffer(device, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

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

            Buffer first(device, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            void* const mapped = first.map();
            const Buffer second = std::move(first);

            EXPECT_EQ(second.map(), mapped) << "the mapping did not come across";
        }
    }
}
