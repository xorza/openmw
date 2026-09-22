#include <algorithm>
#include <cstddef>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/requirements.hpp>

namespace Rtx
{
    namespace
    {
        TEST(RtxRequirementsTest, versionStringSpellsThePackedVersion)
        {
            EXPECT_EQ(versionString(VK_MAKE_API_VERSION(0, 1, 4, 357)), "1.4.357");
            EXPECT_EQ(versionString(sApiVersion), "1.4.0");
            EXPECT_EQ(versionString(VK_MAKE_API_VERSION(0, 0, 0, 0)), "0.0.0");
        }

        /// Every feature structure must be reachable from the head, or `vkGetPhysicalDeviceFeatures2`
        /// leaves it zeroed and the device is rejected over a feature nobody asked it about — with a
        /// message naming the feature rather than the missing link.
        TEST(RtxRequirementsTest, everyFeatureStructIsInTheChain)
        {
            DeviceFeatures features;

            std::set<const void*> linked;
            for (const VkBaseInStructure* next = reinterpret_cast<const VkBaseInStructure*>(&features.mFeatures2);
                 next != nullptr; next = next->pNext)
                linked.insert(next);

            EXPECT_EQ(linked.size(), 12u) << "a member was added to DeviceFeatures without chaining it";
            EXPECT_TRUE(linked.contains(&features.mFeatures2));
            EXPECT_TRUE(linked.contains(&features.mVulkan12));
            EXPECT_TRUE(linked.contains(&features.mVulkan13));
            EXPECT_TRUE(linked.contains(&features.mVulkan14));
            EXPECT_TRUE(linked.contains(&features.mAccelerationStructure));
            EXPECT_TRUE(linked.contains(&features.mRayQuery));
            EXPECT_TRUE(linked.contains(&features.mPositionFetch));
            EXPECT_TRUE(linked.contains(&features.mRayTracingMaintenance1));
            EXPECT_TRUE(linked.contains(&features.mRayTracingPipeline));
            EXPECT_TRUE(linked.contains(&features.mInvocationReorder));
            EXPECT_TRUE(linked.contains(&features.mPipelineExecutable));
        }

        TEST(RtxRequirementsTest, everyPropertyStructIsInTheChain)
        {
            DeviceProperties properties;

            std::set<const void*> linked;
            for (const VkBaseInStructure* next = reinterpret_cast<const VkBaseInStructure*>(&properties.mProperties2);
                 next != nullptr; next = next->pNext)
                linked.insert(next);

            EXPECT_EQ(linked.size(), 6u) << "a member was added to DeviceProperties without chaining it";
            EXPECT_TRUE(linked.contains(&properties.mVulkan11));
            EXPECT_TRUE(linked.contains(&properties.mVulkan12));
            EXPECT_TRUE(linked.contains(&properties.mAccelerationStructure));
            EXPECT_TRUE(linked.contains(&properties.mRayTracingPipeline));
            EXPECT_TRUE(linked.contains(&properties.mInvocationReorder));
        }

        /// A hand-written table of two dozen accessors is where a copy-paste sends two entries at the
        /// same field, and nothing else would notice: both would be requested, both would read as
        /// supported, and the feature one of them was meant to name would never be asked for.
        TEST(RtxRequirementsTest, everyRequiredFeatureAddressesADistinctField)
        {
            DeviceFeatures features;

            std::set<const VkBool32*> seen;
            for (const RequiredFeature& required : getRequiredDeviceFeatures())
                EXPECT_TRUE(seen.insert(&required.mField(features)).second) << required.mName;

            EXPECT_EQ(seen.size(), getRequiredDeviceFeatures().size());
        }

        /// The driver's pacing is optional, both halves of it as one option, and never required: a
        /// card without Reflex traces as it did. Named here so a list edited to require one cannot
        /// pass.
        ///
        /// **And what each option rests on is stated**, which the registry's `depends` says: the
        /// present id rests on a swapchain, and a present fence on a swapchain and the instance's
        /// half of its maintenance. Taken without them, the present id went to every headless
        /// device.
        TEST(RtxRequirementsTest, theOptionsAreTheirExtensionsWholeAndStateWhatTheyRestOn)
        {
            const auto names = [](std::span<const char* const> list) {
                return std::vector<std::string_view>(list.begin(), list.end());
            };
            const std::span<const OptionalExtensions> options = getOptionalExtensions();
            ASSERT_EQ(options.size(), sDeviceOptions);

            const OptionalExtensions& pacing = options[static_cast<std::size_t>(DeviceOption::Pacing)];
            EXPECT_EQ(names(pacing.mExtensions),
                (std::vector<std::string_view>{
                    VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_NV_LOW_LATENCY_2_EXTENSION_NAME }));
            EXPECT_EQ(names(pacing.mNeeds), (std::vector<std::string_view>{ VK_KHR_SWAPCHAIN_EXTENSION_NAME }));

            const OptionalExtensions& fences = options[static_cast<std::size_t>(DeviceOption::PresentFences)];
            EXPECT_EQ(names(fences.mNeeds),
                (std::vector<std::string_view>{
                    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME }));

            for (const DeviceOption alone :
                { DeviceOption::FaultReport, DeviceOption::MemoryBudget, DeviceOption::Checkpoints })
                EXPECT_TRUE(options[static_cast<std::size_t>(alone)].mNeeds.empty())
                    << "an option core 1.4 carries alone names a need";

            const std::span<const char* const> required = getRequiredDeviceExtensions();
            for (const OptionalExtensions& option : options)
                for (const char* const name : option.mExtensions)
                    EXPECT_EQ(std::find_if(required.begin(), required.end(),
                                  [&](const char* const held) { return std::string_view(held) == name; }),
                        required.end())
                        << name << " is both required and optional";
        }

        /// The two directions of the table have to agree: what `requestRequiredFeatures` writes is
        /// exactly what `findMissingFeatures` reads.
        TEST(RtxRequirementsTest, requestingEveryRequiredFeatureLeavesNothingMissing)
        {
            DeviceFeatures features;

            std::vector<std::string_view> missing;
            findMissingFeatures(features, missing);
            EXPECT_EQ(missing.size(), getRequiredDeviceFeatures().size())
                << "a freshly zeroed chain supports nothing, so every entry should be reported";

            requestRequiredFeatures(features);
            missing.clear();
            findMissingFeatures(features, missing);
            EXPECT_TRUE(missing.empty()) << "first still missing: " << (missing.empty() ? "" : missing.front());
        }
    }
}
