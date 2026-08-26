#include <set>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

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
            EXPECT_TRUE(linked.contains(&features.mRayTracingPipeline));
            EXPECT_TRUE(linked.contains(&features.mRayQuery));
            EXPECT_TRUE(linked.contains(&features.mPositionFetch));
            EXPECT_TRUE(linked.contains(&features.mRayTracingMaintenance1));
            EXPECT_TRUE(linked.contains(&features.mOpacityMicromap));
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

            EXPECT_EQ(linked.size(), 7u) << "a member was added to DeviceProperties without chaining it";
            EXPECT_TRUE(linked.contains(&properties.mVulkan11));
            EXPECT_TRUE(linked.contains(&properties.mVulkan12));
            EXPECT_TRUE(linked.contains(&properties.mAccelerationStructure));
            EXPECT_TRUE(linked.contains(&properties.mRayTracingPipeline));
            EXPECT_TRUE(linked.contains(&properties.mOpacityMicromap));
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
