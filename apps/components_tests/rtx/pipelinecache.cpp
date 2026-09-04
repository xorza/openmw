#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
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

            /// What the cache directory holds, by name, sorted so a comparison is against a list and
            /// not against whatever order the filesystem answered in.
            static std::vector<std::string> filesIn(const std::filesystem::path& directory)
            {
                std::vector<std::string> names;
                for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
                    names.push_back(entry.path().filename().string());

                std::sort(names.begin(), names.end());
                return names;
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
        /// The first three are real files that could turn up in a cache directory: a blob from the
        /// other card in a two-card machine, one from before a driver update, and the tail end of a
        /// write that never finished.
        ///
        /// **The offsets below are written out again on purpose.** Sharing the loader's constants
        /// would make this a tautology — an offset wrong in both places agrees with itself and the
        /// test goes green. These are the numbers the specification gives for
        /// `VkPipelineCacheHeaderVersionOne`: the vendor at eight, the UUID at sixteen, and
        /// thirty-two bytes of header in all.
        ///
        /// **The last one is the leg the driver has no opinion about.** A blob this driver wrote is
        /// one it will read back however large it has grown, so that header is the device's own and
        /// the size is the only thing left to refuse it for. What the name does not cover is a file
        /// that is not a cache at all, and one run's own pipelines outgrowing what is worth keeping.
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

        /// The name carries the shaders, and every cache that is not this run's is swept.
        ///
        /// **This is the whole of the eviction, and Vulkan supplies none of it.** A blob cannot be
        /// pruned entry by entry, and nothing in the API has an opinion about a cache full of
        /// pipelines for shaders that no longer exist — so what keeps the directory from holding one
        /// file per driver and per edit for ever is that a run can name exactly one file live and
        /// remove the rest.
        ///
        /// **Two shader sets differing in one byte**, because a digest that ignored the contents
        /// would key both the same and the second run would inherit the first's pipelines: no
        /// symptom at all until a changed shader traced with the old one's code.
        ///
        /// **And a file that is not ours survives**, because a cache directory is shared with
        /// whatever else the game keeps there.
        TEST_F(RtxPipelineCacheTest, theNameCarriesTheShadersAndEveryOtherCacheIsSwept)
        {
            const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "openmw-rtx-cache-test";
            std::filesystem::remove_all(scratch);

            const std::filesystem::path cacheDirectory = scratch / "cache";
            const std::filesystem::path edited = scratch / "edited";
            std::filesystem::create_directories(edited);
            std::filesystem::copy(Testing::getShaderDirectory(), edited, std::filesystem::copy_options::recursive);

            const auto only = [&](const std::filesystem::path& shaders) {
                const PipelineCache cache(getDevice().getHandle(), deviceProperties(),
                    PipelineCacheSpec{ .mDirectory = cacheDirectory, .mShaderDirectory = shaders });
                EXPECT_NE(cache.getHandle(), VK_NULL_HANDLE) << "a cache was made";
            };

            only(Testing::getShaderDirectory());

            std::vector<std::string> after = filesIn(cacheDirectory);
            ASSERT_EQ(after.size(), 1u) << "one run leaves one file";
            const std::string first = after.front();
            EXPECT_TRUE(first.starts_with("rtx-")) << first;

            // A cache from a driver this machine no longer runs, and something that is not ours.
            std::ofstream(cacheDirectory / "rtx-some-other-driver.pipelinecache") << "stale";
            std::ofstream(cacheDirectory / "keep-me.txt") << "not a pipeline cache";

            // One byte of one module, which is what a shader edit comes to.
            const std::filesystem::path module = edited / "tone.comp.spv";
            std::vector<char> bytes;
            {
                std::ifstream reading(module, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(reading), std::istreambuf_iterator<char>());
            }
            ASSERT_FALSE(bytes.empty()) << "there is a module to edit";
            bytes.back() = static_cast<char>(bytes.back() ^ 0xFF);
            std::ofstream(module, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));

            only(edited);

            after = filesIn(cacheDirectory);
            ASSERT_EQ(after.size(), 2u) << "the edited run's cache, and the file that is not ours";
            EXPECT_EQ(after.front(), "keep-me.txt") << "a file this renderer did not write is left alone";
            EXPECT_NE(after.back(), first) << "one byte of one module is a different cache";
            EXPECT_TRUE(after.back().starts_with("rtx-")) << after.back();

            std::filesystem::remove_all(scratch);
        }
    }
}
