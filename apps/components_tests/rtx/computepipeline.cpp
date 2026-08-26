#include <array>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <components/rtx/error.hpp>
#include <components/rtxvulkan/computepipeline.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/validation.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// A pipeline whose shader cannot be opened gives back everything it had already made.
        ///
        /// **The failure comes third.** The set layout and the pipeline layout are live by the time
        /// the module is looked for, and a constructor that throws gets no destructor — so without
        /// the unwind inside `ComputePipeline` both outlive the device. What proves they did not is
        /// the device closing with the layers watching and saying nothing: a live child is what
        /// `vkDestroyDevice` reports.
        ///
        /// Its own device, and closed inside the test, because the suite's is closed after the last
        /// test has run and could not be asked.
        TEST(RtxComputePipelineTest, aMissingShaderLeavesNothingBehindOnTheDevice)
        {
            if (const std::string obstacle = Testing::findInstanceObstacle(); !obstacle.empty())
                GTEST_SKIP() << obstacle;

            InstanceOptions options;
            options.mValidation = true;
            // The throw below is the point of the test, and a leak report is what is being read
            // rather than what should end the run.
            options.mPolicy = ValidationPolicy::Log;

            const Instance instance(options);
            auto device = std::make_unique<Device>(instance, PhysicalDevice::select(instance.getHandle()));

            ValidationLog* log = instance.getValidationLog();
            ASSERT_NE(log, nullptr) << "the layers are what this test reads its answer from";

            constexpr std::array<VkDescriptorSetLayoutBinding, 1> bindings{
                VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            };
            // Any non-zero push-constant size does; a range of zero is not a legal one to ask for.
            EXPECT_THROW(ComputePipeline(*device, bindings, sizeof(float), {}, "no-such.comp.spv", "scratch"), Error);

            log->clear();
            device.reset();

            for (const ValidationMessage& message : log->getErrorsOnThisThread())
                ADD_FAILURE() << "validation error at device teardown: " << message.mText;
        }
    }
}
