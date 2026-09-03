#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/pipelinecache.hpp>
#include <components/rtxvulkan/requirements.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxPipelineCacheTest : Testing::DeviceTest
        {
            /// The blob the driver would save, which every leg below is built from.
            std::vector<std::uint8_t> deviceBlob()
            {
                const Device& device = getDevice();

                std::size_t bytes = 0;
                EXPECT_EQ(
                    vkGetPipelineCacheData(device.getHandle(), device.getPipelineCache(), &bytes, nullptr), VK_SUCCESS);

                std::vector<std::uint8_t> blob(bytes);
                EXPECT_EQ(vkGetPipelineCacheData(device.getHandle(), device.getPipelineCache(), &bytes, blob.data()),
                    VK_SUCCESS);

                return blob;
            }

            const VkPhysicalDeviceProperties& deviceProperties()
            {
                return getDevice().getPhysicalDevice().getProperties().mProperties2.properties;
            }
        };

        /// The device has a cache, and what it writes is what the loader will take back.
        ///
        /// **The check the loader applies has to accept the driver's own output**, and there is
        /// nothing about the arithmetic that says so — the header's fields are read at hard-coded
        /// offsets, and one of them off by four would reject every blob ever written. The symptom
        /// would be a cache that silently never hit: no error, no warning, only a test suite slowly
        /// getting slower again and a shader compiled once a process forever.
        ///
        /// So this asks the driver for the blob it would save and runs it through the same door it
        /// would come back in.
        TEST_F(RtxPipelineCacheTest, whatTheDriverWritesIsWhatTheLoaderTakesBack)
        {
            ASSERT_NE(getDevice().getPipelineCache(), VK_NULL_HANDLE) << "the device made a pipeline cache";

            const std::vector<std::uint8_t> blob = deviceBlob();

            // Even a cache nothing was compiled into carries its header, which is all this reads.
            ASSERT_GE(blob.size(), std::size_t{ 32 }) << "a blob is at least a header";

            EXPECT_TRUE(PipelineCache::accepts(blob, deviceProperties())) << "this driver's own blob";
        }

        /// And it refuses what another machine wrote, what a dead process left half written, and
        /// what has grown past keeping.
        ///
        /// **The negative leg, because a check that accepted everything would pass the one above.**
        /// The first three are real files that could turn up in a shared temporary directory: a blob
        /// from the other card in a two-card machine, one from before a driver update, and the tail
        /// end of a write that never finished.
        ///
        /// **The offsets below are written out again on purpose.** Sharing the loader's constants
        /// would make this a tautology — an offset wrong in both places agrees with itself and the
        /// test goes green. These are the numbers the specification gives for
        /// `VkPipelineCacheHeaderVersionOne`: the vendor at eight, the UUID at sixteen, and
        /// thirty-two bytes of header in all.
        ///
        /// **The last one is the leg the driver has no opinion about.** A blob this driver wrote is
        /// one it will read back however large it has grown, and it grows by a build's worth of
        /// pipelines at every shader edit — so that header is the device's own, and the size is the
        /// only thing left to refuse it for.
        TEST_F(RtxPipelineCacheTest, aBlobThisRunWillNotSeedFromIsRefused)
        {
            const std::vector<std::uint8_t> blob = deviceBlob();
            const VkPhysicalDeviceProperties& properties = deviceProperties();
            ASSERT_TRUE(PipelineCache::accepts(blob, properties));

            // Another vendor's, at byte eight of the header.
            std::vector<std::uint8_t> elsewhere = blob;
            elsewhere.at(8) ^= 0xFF;
            EXPECT_FALSE(PipelineCache::accepts(elsewhere, properties)) << "another vendor";

            // The same driver after an update, which is what the UUID is for.
            std::vector<std::uint8_t> updated = blob;
            updated.at(16) ^= 0xFF;
            EXPECT_FALSE(PipelineCache::accepts(updated, properties)) << "another driver build";

            // And a write that stopped part way through the header itself.
            EXPECT_FALSE(PipelineCache::accepts(std::span(blob).first(31), properties)) << "a torn header";
            EXPECT_FALSE(PipelineCache::accepts({}, properties)) << "nothing at all";

            std::vector<std::uint8_t> grown(PipelineCache::sMostBytes + 1, 0);
            std::copy(blob.begin(), blob.end(), grown.begin());
            EXPECT_FALSE(PipelineCache::accepts(grown, properties)) << "one byte past what is kept";

            grown.resize(PipelineCache::sMostBytes);
            EXPECT_TRUE(PipelineCache::accepts(grown, properties)) << "exactly what is kept";
        }
    }
}
