#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/error.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/handles.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/requirements.hpp>
#include <components/rtxvulkan/result.hpp>

#include "support/device/harness.hpp"

namespace Rtx
{
    namespace
    {
        /// Nothing of its own — a name, because `TEST_F` takes one identifier and prints it as the
        /// suite these tests are reported under.
        using RtxDeviceTest = Testing::DeviceTest;

        /// A wait on a device that never answers ends, and says which wait it was.
        ///
        /// **The alternative cannot be told from success.** `vkWaitForFences` with no timeout makes a
        /// device that will never signal and one still working the same call, and a stalled submit
        /// took the whole suite with it — a wedged process, a GPU at full tilt, and no message. Every
        /// wait in this renderer now has a deadline; this is the one that proves the deadline fires
        /// rather than being a number nobody has ever reached.
        ///
        /// A fence nothing submits against, and a patience short enough that the test does not sit
        /// out the real one.
        TEST_F(RtxDeviceTest, aWaitOnADeviceThatNeverAnswersEndsAndNamesItself)
        {
            const VkFenceCreateInfo unsignalled{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

            VkFence fence = VK_NULL_HANDLE;
            ASSERT_EQ(vkCreateFence(mHarness.mDevice->getHandle(), &unsignalled, nullptr, &fence), VK_SUCCESS);

            try
            {
                awaitVk(*mHarness.mDevice, fence, "a submit nobody made", 1'000'000ull);
                ADD_FAILURE() << "the wait returned, so a device that never answers still looks like success";
            }
            catch (const DeviceError& e)
            {
                // Named, because a count says a frame is stuck and nothing about which one.
                EXPECT_NE(std::string(e.what()).find("a submit nobody made"), std::string::npos) << e.what();
                EXPECT_NE(std::string(e.what()).find("stopped answering"), std::string::npos) << e.what();
            }

            vkDestroyFence(mHarness.mDevice->getHandle(), fence, nullptr);
        }

        TEST_F(RtxDeviceTest, theValidationLayerIsLoaded)
        {
            // Without this every other test's clean bill of health means nothing.
            EXPECT_NE(mHarness.mInstance->getValidationLog(), nullptr);
        }

        TEST_F(RtxDeviceTest, theDeviceHasAQueueAndEveryExtensionEntryPoint)
        {
            EXPECT_NE(mHarness.mDevice->getHandle(), VK_NULL_HANDLE);
            EXPECT_NE(mHarness.mDevice->getQueue(), VK_NULL_HANDLE);

            // Device construction throws when any of these is missing, so reaching here already
            // proves it; asserting names the contract for anyone reading the failure.
            const DeviceFunctions& functions = mHarness.mDevice->getFunctions();
            EXPECT_NE(functions.mCmdBuildAccelerationStructures, nullptr);
            EXPECT_NE(functions.mGetAccelerationStructureDeviceAddress, nullptr);
            EXPECT_NE(functions.mGetAccelerationStructureBuildSizes, nullptr);

            // The one optional entry point, present exactly where the driver offers its extension.
            // An extension enabled and never read is what this proves gone.
            const PhysicalDevice& physical = mHarness.mDevice->getPhysicalDevice();
            EXPECT_EQ(mHarness.mDevice->canDescribeFault(),
                physical.hasOptionalExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME));
        }

        /// The device and the renderer are made without a validation error. What the layers raise
        /// while either is made reaches no test's own drain, which takes whatever is on the log for
        /// a previous test's and drops it.
        TEST_F(RtxDeviceTest, theDeviceAndTheRendererAreMadeWithoutAValidationError)
        {
            for (const std::string& error : mHarness.mMadeWith)
                ADD_FAILURE() << "making the device: " << error;

            // Made by the first ask, which may be this one.
            Testing::getRenderer();
            for (const std::string& error : Testing::getRendererMadeWith())
                ADD_FAILURE() << "making the renderer: " << error;
        }

        /// A device with no window takes no option that rests on a swapchain, whatever the driver
        /// offers: no present id and no pacing on it, and no present fence. The test's device is
        /// made on an instance with no surface, as every headless run's is.
        TEST_F(RtxDeviceTest, aDeviceWithNoWindowTakesNoOptionThatRestsOnASwapchain)
        {
            ASSERT_FALSE(mHarness.mInstance->hasExtension(VK_KHR_SURFACE_EXTENSION_NAME));

            EXPECT_FALSE(mHarness.mDevice->hasLatencyPacing());
            EXPECT_FALSE(mHarness.mDevice->hasPresentFences());
        }

        TEST_F(RtxDeviceTest, everyRequiredFeatureIsActuallySupported)
        {
            DeviceFeatures supported;
            vkGetPhysicalDeviceFeatures2(mHarness.mDevice->getPhysicalDevice().getHandle(), &supported.mFeatures2);

            std::vector<std::string_view> missing;
            findMissingFeatures(supported, missing);

            EXPECT_TRUE(missing.empty()) << "first missing: " << (missing.empty() ? "" : missing.front());
        }

        TEST_F(RtxDeviceTest, theShaderBuildStepProducesLoadableModules)
        {
            const std::filesystem::path visibility = Testing::getShaderDirectory() / "visibility.rgen.spv";
            ASSERT_TRUE(std::filesystem::exists(visibility)) << visibility;

            const ShaderModule module = loadShaderModule(*mHarness.mDevice, visibility);
            EXPECT_NE(module.get(), VK_NULL_HANDLE);
        }

        TEST_F(RtxDeviceTest, aFileThatIsNotSpirvIsRejectedRatherThanHandedToTheDriver)
        {
            const std::filesystem::path missing = Testing::getShaderDirectory() / "there-is-no-such-shader.spv";
            EXPECT_THROW(loadShaderModule(*mHarness.mDevice, missing), InputError);
        }

        TEST_F(RtxDeviceTest, theReportNamesTheDeviceAndItsRayTracingLimits)
        {
            const std::string report = mHarness.mDevice->getPhysicalDevice().describe();

            EXPECT_NE(
                report.find(mHarness.mDevice->getPhysicalDevice().getProperties().mProperties2.properties.deviceName),
                std::string::npos);
            EXPECT_NE(report.find("max primitive count"), std::string::npos);
        }
    }
}
