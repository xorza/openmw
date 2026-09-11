#include <array>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/error.hpp>
#include <components/rtxvulkan/computepipeline.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/pipelinecache.hpp>
#include <components/rtxvulkan/validation.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// Nothing of its own — a name, because `TEST_F` takes one identifier and prints it as the
        /// suite this test is reported under.
        using RtxComputePipelineTest = Testing::DeviceTest;

        /// A pipeline whose shader cannot be opened gives back everything it had already made.
        ///
        /// **The failure comes third.** The set layout and the pipeline layout are live by the time
        /// the module is looked for, and a constructor that throws gets no destructor — so without
        /// the unwind inside `ComputePipeline` both outlive the device. What proves they did not is
        /// the device closing with the layers watching and saying nothing: a live child is what
        /// `vkDestroyDevice` reports.
        ///
        /// A device of its own, closed inside the test, because the suite's is closed after the last
        /// test has run and could not be asked.
        ///
        /// **On the suite's instance, and with no pipeline cache.** A second instance loads the
        /// layers again for an answer this already has, and a cache is a quarter of a gigabyte read
        /// and written for a test whose one pipeline never compiles.
        TEST_F(RtxComputePipelineTest, aMissingShaderLeavesNothingBehindOnTheDevice)
        {
            const Instance& instance = *mHarness->mInstance;
            auto device
                = std::make_unique<Device>(instance, PhysicalDevice::select(instance.getHandle()), PipelineCacheSpec{});

            ValidationLog* log = instance.getValidationLog();
            ASSERT_NE(log, nullptr) << "the layers are what this test reads its answer from";

            constexpr std::array<VkDescriptorSetLayoutBinding, 1> bindings{
                VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            };
            // Any non-zero push-constant size does; a range of zero is not a legal one to ask for.
            EXPECT_THROW(ComputePipeline(*device, bindings, sizeof(float), {}, "no-such.comp.spv", "scratch"), Error);

            log->clear();
            device.reset();

            std::vector<std::string> raised;
            log->takeErrorsOnThisThread(raised);
            for (const std::string& message : raised)
                ADD_FAILURE() << "validation error at device teardown: " << message;
        }
    }
}
