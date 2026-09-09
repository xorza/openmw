#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxvulkan/deviceprofile.hpp>
#include <components/rtxvulkan/requirements.hpp>

namespace Rtx
{
    namespace
    {
        /// A queue family that can do everything this renderer submits, and can time it.
        constexpr VkQueueFamilyProperties sWholeQueue{
            .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
            .queueCount = 16,
            .timestampValidBits = 64,
        };

        /// The extensions this renderer requires, as a device would list them back.
        std::vector<std::string> everyRequiredExtension()
        {
            std::vector<std::string> names;
            for (const char* const required : getRequiredDeviceExtensions())
                names.emplace_back(required);

            return names;
        }

        /// An RTX 2060 as the Vulkan Hardware Database reports it: report 46422, driver 590.48.1 on
        /// Linux, Vulkan 1.4.325.
        ///
        /// **A card nobody here owns, described from what it says of itself.** Three heaps — six
        /// gigabytes of video memory, twenty-five of system memory, and a 246 MiB window the host
        /// writes into — and a reordering hint of `NONE`, which is what every RTX card before Ada
        /// answers.
        void describeTuring(DeviceProperties& properties)
        {
            properties.mProperties2.properties.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 325);
            properties.mInvocationReorder.rayTracingInvocationReorderReorderingHint
                = VK_RAY_TRACING_INVOCATION_REORDER_MODE_NONE_EXT;

            VkPhysicalDeviceMemoryProperties& memory = properties.mMemory;
            memory.memoryHeapCount = 3;
            memory.memoryHeaps[0] = VkMemoryHeap{ 6442450944ull, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
            memory.memoryHeaps[1] = VkMemoryHeap{ 25177847808ull, 0 };
            memory.memoryHeaps[2] = VkMemoryHeap{ 257949696ull, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };

            memory.memoryTypeCount = 5;
            memory.memoryTypes[0] = VkMemoryType{ 0, 1 };
            memory.memoryTypes[1] = VkMemoryType{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
            memory.memoryTypes[2]
                = VkMemoryType{ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1 };
            memory.memoryTypes[3] = VkMemoryType{ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                1 };
            memory.memoryTypes[4] = VkMemoryType{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                    | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                2 };
        }

        /// This box's RTX 4090 Laptop, as `openmw-rtxtool info` reports it.
        ///
        /// **One heap of video memory, host-visible throughout**, which is what resizable BAR makes
        /// of a card — so the aperture the profile measures is the whole of it.
        void describeAda(DeviceProperties& properties)
        {
            properties.mProperties2.properties.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 341);
            properties.mInvocationReorder.rayTracingInvocationReorderReorderingHint
                = VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_EXT;

            VkPhysicalDeviceMemoryProperties& memory = properties.mMemory;
            memory.memoryHeapCount = 2;
            memory.memoryHeaps[0] = VkMemoryHeap{ 17171480576ull, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
            memory.memoryHeaps[1] = VkMemoryHeap{ 50259238912ull, 0 };

            memory.memoryTypeCount = 4;
            memory.memoryTypes[0] = VkMemoryType{ 0, 1 };
            memory.memoryTypes[1] = VkMemoryType{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
            memory.memoryTypes[2]
                = VkMemoryType{ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1 };
            memory.memoryTypes[3] = VkMemoryType{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                    | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                0 };
        }

        /// Everything a qualifying device answers, so a case below changes one thing and asks what
        /// the profile makes of it.
        struct Card
        {
            explicit Card(void (*describe)(DeviceProperties&))
                : mExtensions(everyRequiredExtension())
                , mQueues{ sWholeQueue }
            {
                describe(mProperties);
                requestRequiredFeatures(mFeatures);
            }

            DeviceProfile profile() { return profileOf(mProperties, mFeatures, mExtensions, mQueues); }

            DeviceProperties mProperties;
            DeviceFeatures mFeatures;
            std::vector<std::string> mExtensions;
            std::vector<VkQueueFamilyProperties> mQueues;
        };

        /// Two cards this fork targets, and the profile differs in exactly what their hardware does.
        ///
        /// **The whole reason the type exists.** Neither card is plugged into this machine, and both
        /// answers matter: an RTX 2060 that reorders nothing runs the same trace, and its 246 MiB
        /// aperture is what decides where the scene's tables live.
        TEST(RtxDeviceProfileTest, twoCardsDifferExactlyWhereTheirHardwareDoes)
        {
            Card turing(&describeTuring);
            Card ada(&describeAda);

            const DeviceProfile onTuring = turing.profile();
            const DeviceProfile onAda = ada.profile();

            EXPECT_EQ(onTuring.mObstacle, "") << "an RTX 2060 was refused";
            EXPECT_EQ(onAda.mObstacle, "") << "an RTX 4090 was refused";

            EXPECT_FALSE(onTuring.mReorders);
            EXPECT_TRUE(onAda.mReorders);

            // 246 MiB against the whole of video memory, which is the difference resizable BAR
            // makes. `DeviceProfile::mHostWrittenBytes` says what the figure then decides.
            EXPECT_EQ(onTuring.mHostWrittenBytes, 257949696ull);
            EXPECT_EQ(onAda.mHostWrittenBytes, 17171480576ull);
            EXPECT_NE(onTuring.mHostWrittenBytes, onAda.mHostWrittenBytes);

            // What both agree on, so a difference above is the hardware's and not the fixture's.
            EXPECT_EQ(onTuring.mQueueFamily, 0u);
            EXPECT_EQ(onAda.mQueueFamily, 0u);
            EXPECT_EQ(onTuring.mTimestampBits, 64u);
        }

        /// An optional extension is taken where the device lists it and left where it does not.
        ///
        /// **Neither answer refuses the device**, which is the whole difference between this list
        /// and the required one: a card without `VK_EXT_device_fault` traces the same frames and
        /// says less about a device loss.
        TEST(RtxDeviceProfileTest, anOptionalExtensionIsTakenOnlyWhereTheDeviceListsIt)
        {
            Card bare(&describeTuring);
            EXPECT_TRUE(bare.profile().mOptionalExtensions.empty()) << "an extension nothing offered was taken";

            Card full(&describeTuring);
            for (const char* const name : getOptionalDeviceExtensions())
                full.mExtensions.emplace_back(name);

            const DeviceProfile profile = full.profile();
            ASSERT_EQ(profile.mOptionalExtensions.size(), getOptionalDeviceExtensions().size());
            for (std::size_t at = 0; at < profile.mOptionalExtensions.size(); ++at)
                EXPECT_STREQ(profile.mOptionalExtensions[at], getOptionalDeviceExtensions()[at])
                    << "the order the list states was not kept";

            EXPECT_EQ(profile.mObstacle, "") << "an optional extension decided whether the card qualifies";
        }

        /// Each thing a device can lack is named, and nothing else is.
        ///
        /// **One case per obstacle, on a card that qualifies but for that one thing**, so a message
        /// naming the wrong lack fails here rather than sending a reader after it.
        TEST(RtxDeviceProfileTest, aCardShortOfOneThingIsNamedForThatThing)
        {
            {
                Card old(&describeTuring);
                old.mProperties.mProperties2.properties.apiVersion = VK_API_VERSION_1_3;
                EXPECT_EQ(old.profile().mObstacle, "reports Vulkan 1.3.0");
            }
            {
                Card short_(&describeTuring);
                short_.mExtensions.erase(short_.mExtensions.begin());
                EXPECT_EQ(short_.profile().mObstacle,
                    "missing extensions: " + std::string(getRequiredDeviceExtensions().front()));
            }
            {
                Card short_(&describeTuring);
                const RequiredFeature& first = getRequiredDeviceFeatures().front();
                first.mField(short_.mFeatures) = VK_FALSE;
                EXPECT_EQ(short_.profile().mObstacle, "missing features: " + std::string(first.mName));
            }
            {
                Card split(&describeTuring);
                split.mQueues.front().queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
                EXPECT_EQ(split.profile().mObstacle, "no queue family with graphics, compute and transfer");
            }
            {
                // The whole of what makes such a card unusable: `Buffer::hostWritten` has nowhere to
                // go, and nothing in this renderer stages instead.
                Card walled(&describeTuring);
                walled.mProperties.mMemory.memoryTypeCount = 4;
                const DeviceProfile profile = walled.profile();
                EXPECT_EQ(profile.mHostWrittenBytes, 0u);
                EXPECT_EQ(profile.mObstacle, "no memory type the host writes into and the device reads");
            }
        }

        /// A second aperture does not add to the first.
        ///
        /// **A buffer goes in one heap.** Two windows of 128 MiB hold no table a single 256 MiB one
        /// would not, so the profile reports the largest rather than the sum — and a sum would say a
        /// card had room it has nowhere.
        TEST(RtxDeviceProfileTest, twoAperturesReportTheLargerAndNotTheirSum)
        {
            Card split(&describeTuring);
            VkPhysicalDeviceMemoryProperties& memory = split.mProperties.mMemory;

            memory.memoryHeaps[2].size = 134217728ull;
            memory.memoryHeaps[memory.memoryHeapCount] = VkMemoryHeap{ 201326592ull, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
            memory.memoryTypes[memory.memoryTypeCount] = VkMemoryType{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                    | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                memory.memoryHeapCount };
            ++memory.memoryHeapCount;
            ++memory.memoryTypeCount;

            EXPECT_EQ(split.profile().mHostWrittenBytes, 201326592ull) << "two apertures were added together";
        }
    }
}
