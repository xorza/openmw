#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/requirements.hpp>
#include <components/rtxvulkan/upscaler.hpp>

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

        /// The extensions this build requires — the trace's and its upscaler's — as a device would
        /// list them back.
        std::vector<std::string> everyRequiredExtension()
        {
            std::vector<std::string> names;
            for (const RequiredExtension& required : getRequiredDeviceExtensions())
                names.emplace_back(required.mName);
            for (const char* const needed : upscalerDeviceExtensions())
                names.emplace_back(needed);

            return names;
        }

        /// An RTX 2060 as the Vulkan Hardware Database reports it: report 46422, driver 590.48.01 on
        /// Linux, Vulkan 1.4.325.
        ///
        /// **A card nobody here owns, described from what it says of itself.** Three heaps — six
        /// gigabytes of video memory, twenty-five of system memory, and a 246 MiB window the host
        /// writes into — and a reordering hint of `NONE`, which is what every RTX card before Ada
        /// answers. Its driver lists `VK_NV_ray_tracing_invocation_reorder` and not the `EXT` one,
        /// so the card as reported is refused; a `Card` lists every required extension, as any
        /// driver from 595 does, and `aCardShortOfOneThingIsNamedForThatThing` puts the report's
        /// own list back.
        void describeTuring(DeviceProperties& properties)
        {
            properties.mProperties2.properties.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 325);
            properties.mVulkan12.driverID = VK_DRIVER_ID_NVIDIA_PROPRIETARY;
            std::ranges::copy(std::string_view("590.48.01"), properties.mVulkan12.driverInfo);
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
            properties.mVulkan12.driverID = VK_DRIVER_ID_NVIDIA_PROPRIETARY;
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

                // Every required format offered whole, in optimal tiling, which is where an image
                // the trace samples lives.
                for (const RequiredFormat& required : getRequiredFormats())
                    mFormats.push_back(VkFormatProperties{ .optimalTilingFeatures = required.mFeatures });
            }

            PhysicalDevice::Profile profile()
            {
                return PhysicalDevice::profileOf(mProperties, mFeatures, mExtensions, mQueues, mFormats);
            }

            DeviceProperties mProperties;
            DeviceFeatures mFeatures;
            std::vector<std::string> mExtensions;
            std::vector<VkQueueFamilyProperties> mQueues;
            std::vector<VkFormatProperties> mFormats;
        };

        /// Two cards this fork targets, and the profile differs in exactly what their hardware does.
        ///
        /// **The whole reason the type exists.** Neither card is asked, only described, and both
        /// answers matter: an RTX 2060 that reorders nothing runs the same trace, and its 246 MiB
        /// aperture is what decides where the scene's tables live.
        TEST(RtxPhysicalDeviceTest, twoCardsDifferExactlyWhereTheirHardwareDoes)
        {
            Card turing(&describeTuring);
            Card ada(&describeAda);

            const PhysicalDevice::Profile onTuring = turing.profile();
            const PhysicalDevice::Profile onAda = ada.profile();

            EXPECT_EQ(onTuring.mObstacle, "") << "an RTX 2060 was refused";
            EXPECT_EQ(onAda.mObstacle, "") << "an RTX 4090 was refused";

            // 246 MiB against the whole of video memory, which is the difference resizable BAR
            // makes. `PhysicalDevice::Profile::mHostWrittenBytes` says what the figure then decides.
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
        TEST(RtxPhysicalDeviceTest, anOptionalExtensionIsTakenOnlyWhereTheDeviceListsIt)
        {
            Card bare(&describeTuring);
            EXPECT_TRUE(bare.profile().mOptionalExtensions.empty()) << "an extension nothing offered was taken";

            // Every option's extensions, in the table's order.
            std::vector<const char*> every;
            for (const OptionalExtensions& option : getOptionalExtensions())
                every.insert(every.end(), option.mExtensions.begin(), option.mExtensions.end());

            Card full(&describeTuring);
            for (const char* const name : every)
                full.mExtensions.emplace_back(name);

            const PhysicalDevice::Profile profile = full.profile();
            ASSERT_EQ(profile.mOptionalExtensions.size(), every.size());
            for (std::size_t at = 0; at < profile.mOptionalExtensions.size(); ++at)
                EXPECT_STREQ(profile.mOptionalExtensions[at], every[at]) << "the order the table states was not kept";

            EXPECT_EQ(profile.mObstacle, "") << "an optional extension decided whether the card qualifies";
        }

        /// Each thing a device can lack is named, and nothing else is.
        ///
        /// **One case per obstacle, on a card that qualifies but for that one thing**, so a message
        /// naming the wrong lack fails here rather than sending a reader after it.
        TEST(RtxPhysicalDeviceTest, aCardShortOfOneThingIsNamedForThatThing)
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
                    "missing extensions: " + std::string(getRequiredDeviceExtensions().front().mName));
            }
            {
                // Report 46422's own list: the card is short of a driver and not of hardware, and
                // the refusal names the release that has the extension beside the one that has not.
                Card dated(&describeTuring);
                std::erase(dated.mExtensions, VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
                dated.mExtensions.emplace_back(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
                EXPECT_EQ(dated.profile().mObstacle,
                    "missing extensions: VK_EXT_ray_tracing_invocation_reorder (NVIDIA driver 595 or later; this "
                    "one is 590.48.01)");

                // NVIDIA's releases say nothing of another vendor's driver on the same card.
                dated.mProperties.mVulkan12.driverID = VK_DRIVER_ID_MESA_NVK;
                EXPECT_EQ(dated.profile().mObstacle, "missing extensions: VK_EXT_ray_tracing_invocation_reorder");
            }
            {
                // What a Radeon or an Arc lists — reports 51246 and 51371: everything the trace needs
                // and neither of the NVIDIA extensions NGX asks for. A build with DLSS cannot make a
                // device of it, and a build without one traces on it.
                Card foreign(&describeTuring);
                foreign.mProperties.mVulkan12.driverID = VK_DRIVER_ID_AMD_PROPRIETARY;
                std::erase(foreign.mExtensions, VK_NVX_BINARY_IMPORT_EXTENSION_NAME);
                std::erase(foreign.mExtensions, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME);
#ifdef OPENMW_RTX_DLSS
                EXPECT_EQ(foreign.profile().mObstacle,
                    "missing extensions DLSS Ray Reconstruction needs: VK_NVX_binary_import, VK_NVX_image_view_handle");
#else
                EXPECT_EQ(foreign.profile().mObstacle, "");
#endif
            }
            {
                // NGX names the buffer address extension, whose feature is core here and which the
                // spec forbids beside it: a device need not list what it is never asked to enable.
                Card core(&describeTuring);
                std::erase(core.mExtensions, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
                EXPECT_EQ(core.profile().mObstacle, "");
            }
            {
                Card short_(&describeTuring);
                const RequiredFeature& first = getRequiredDeviceFeatures().front();
                first.mField(short_.mFeatures) = VK_FALSE;
                EXPECT_EQ(short_.profile().mObstacle, "missing features: " + std::string(first.mName));
            }
            {
                // Offered for linear tiling only, which is not where an image the trace samples
                // lives, and so not offered.
                Card flat(&describeTuring);
                flat.mFormats.front()
                    = VkFormatProperties{ .linearTilingFeatures = getRequiredFormats().front().mFeatures };
                EXPECT_EQ(flat.profile().mObstacle,
                    "missing format features for " + std::string(getRequiredFormats().front().mFor));
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
                const PhysicalDevice::Profile profile = walled.profile();
                EXPECT_EQ(profile.mHostWrittenBytes, 0u);
                EXPECT_EQ(profile.mObstacle, "no memory type the host writes into and the device reads");
            }
        }

        /// A second aperture does not add to the first.
        ///
        /// **A buffer goes in one heap.** Two windows of 128 MiB hold no table a single 256 MiB one
        /// would not, so the profile reports the largest rather than the sum — and a sum would say a
        /// card had room it has nowhere.
        TEST(RtxPhysicalDeviceTest, twoAperturesReportTheLargerAndNotTheirSum)
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
