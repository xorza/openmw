#include "harness.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/files/configurationmanager.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/validation.hpp>
#include <components/rtxvulkan/vulkanrenderer.hpp>

#include "../instanceobstacle.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// Keyed on validation, which is the only axis any of these vary along.
        Once<Harness>& harnessCache(bool validation)
        {
            static Once<Harness> sValidated;
            static Once<Harness> sPlain;
            return validation ? sValidated : sPlain;
        }

        /// Keyed on validation, the way the devices are and for the same reason.
        Once<VulkanRenderer>& rendererCache(bool validation)
        {
            static Once<VulkanRenderer> sValidated;
            static Once<VulkanRenderer> sPlain;
            return validation ? sValidated : sPlain;
        }

        std::unique_ptr<Harness> build(bool validation)
        {
            if (const std::string obstacle = findInstanceObstacle(); !obstacle.empty())
                throw std::runtime_error(obstacle);

            // Tests provoke errors deliberately and assert on them; aborting would take the suite
            // down with the first one. Synchronization validation is **the same switch
            // `describeRenderer` sets, for the same reason**: a test that drives Vulkan directly
            // supplies its own ordering with a submit and a wait, so a missing barrier in the code
            // under it shows as nothing at all — and a suite validated one way through the renderer
            // and another way beside it answers a different question in each file. It costs no
            // measurable time here either.
            const ValidationOptions options{
                .mLevel = validation ? ValidationLevel::Sync : ValidationLevel::Off,
                .mAbortOnError = false,
            };

            auto harness = std::make_unique<Harness>();
            harness->mInstance = std::make_unique<Instance>(options, std::span<const char* const>{});

            std::uint32_t count = 0;
            if (vkEnumeratePhysicalDevices(harness->mInstance->getHandle(), &count, nullptr) != VK_SUCCESS
                || count == 0)
                throw std::runtime_error("no Vulkan device is installed");

            harness->mDevice = std::make_unique<Device>(
                *harness->mInstance, PhysicalDevice::select(harness->mInstance->getHandle()), getPipelineCacheSpec());
            if (ValidationLog* log = harness->mInstance->getValidationLog(); log != nullptr)
                log->takeErrorsOnThisThread(harness->mMadeWith);

            return harness;
        }

        /// **The flag reaches the cache and the build together**, so the two cannot come apart: a
        /// device built unvalidated and filed under the validated key would be handed to every test
        /// in the suite.
        Harness& cachedHarness(bool validation)
        {
            return harnessCache(validation).get([validation] { return build(validation); });
        }

        /// What the layers raised while the validated renderer was made: the other loads none.
        std::vector<std::string>& rendererMadeWith()
        {
            static std::vector<std::string> sMadeWith;
            return sMadeWith;
        }

        std::unique_ptr<VulkanRenderer> buildRenderer(bool validation)
        {
            // Every test resizes to what it needs; one texel is only what the first target costs.
            auto renderer = std::make_unique<VulkanRenderer>(describeRenderer(1, 1, validation));
            if (validation)
                renderer->takeValidationErrors(rendererMadeWith());
            return renderer;
        }

        VulkanRenderer& cachedRenderer(bool validation)
        {
            return rendererCache(validation).get([validation] { return buildRenderer(validation); });
        }

        /// What holding a device for the run costs the rest of the binary: death tests that exec
        /// rather than fork, and every device closed after the last test and before `main` returns.
        ///
        /// **A fork of a process holding a device runs to seconds**: its mappings are copied and
        /// the driver's fork handlers run, and gtest's default death test is a fork. Measured at
        /// four to eight seconds a death test once the pixel suite had built a renderer, against
        /// a fifth of a second for the re-exec the `threadsafe` style does — a child that never
        /// held a device.
        ///
        /// **Two Vulkan devices destroyed after `main` has returned abort inside the validation
        /// layer**, with no message and no stack of ours on it. One pair survives static destruction
        /// and a second does not — reproduced with nothing in the process but two instances left to
        /// exit — and this binary keeps up to four: a raw device for the tests that drive Vulkan
        /// directly and a `Renderer` for the pixel suite, each in a validated and an unvalidated
        /// flavour. Closing them here is both the fix and where they belonged: a cache that lives
        /// for the run should end with the run, not with the process.
        ///
        /// **And no device is a failed run, not an empty one.** A skip per test is honest per test
        /// and a green run of nothing per suite. This binary holds only the tests that need a device,
        /// so the first thing it does is ask for it, and every fixture after it holds a reference.
        ///
        /// **The device and not the renderer**, because a death test's child runs this again: the
        /// renderer costs such a child three and a half seconds, and a device `PhysicalDevice::select`
        /// accepted is one the renderer is built on. A renderer that still cannot be built throws
        /// out of `getRenderer`, which fails the test that asked.
        class DeviceEnvironment : public ::testing::Environment
        {
            void SetUp() override
            {
                GTEST_FLAG_SET(death_test_style, "threadsafe");

                try
                {
                    getHarness();
                }
                catch (const std::exception& obstacle)
                {
                    FAIL() << "rtx-gpu-tests needs a device and this machine has none: " << obstacle.what();
                }
            }

            void TearDown() override
            {
                // The renderer before the raw devices, which is the order they were built in; neither
                // depends on the other.
                for (const bool validation : { true, false })
                    rendererCache(validation).release();
                for (const bool validation : { true, false })
                    harnessCache(validation).release();
            }
        };

        // Before `main`, because gtest only tears down environments registered before the run starts.
        [[maybe_unused]] const bool sRegistered = [] {
            ::testing::AddGlobalTestEnvironment(new DeviceEnvironment);
            return true;
        }();
    }

    Harness& getHarness()
    {
        return cachedHarness(true);
    }

    Harness& getUnvalidatedHarness()
    {
        return cachedHarness(false);
    }

    std::filesystem::path getShaderDirectory()
    {
        return std::filesystem::path(OPENMW_RTX_SHADER_DIR);
    }

    PipelineCacheSpec getPipelineCacheSpec()
    {
        // Silent, and built once: what is wanted is the path rule and not a configuration, and this
        // constructor reads no files to answer it.
        static const std::filesystem::path directory = Files::ConfigurationManager(true).getCachePath();

        return PipelineCacheSpec{ .mDirectory = directory, .mShaderDirectory = getShaderDirectory() };
    }

    RendererOptions describeRenderer(std::uint32_t width, std::uint32_t height, bool validation)
    {
        RendererOptions options;
        options.mShaderDirectory = getShaderDirectory();
        options.mCacheDirectory = getPipelineCacheSpec().mDirectory;
        options.mWidth = width;
        options.mHeight = height;
        // **Synchronization validation wherever the layers are, because a missing barrier is what
        // this suite is worst at seeing.** Every test here submits and waits, so the ordering a
        // frame relies on is supplied by the harness rather than by the code under test, and a
        // hazard shows as nothing at all. It costs no measurable time in this suite.
        options.mValidation.mLevel = validation ? ValidationLevel::Sync : ValidationLevel::Off;
        // Tests provoke errors deliberately and assert on them; aborting would take the suite down
        // with the first one.
        options.mValidation.mAbortOnError = false;
        // One, because a measured exposure makes every pixel depend on the whole frame's histogram,
        // and a test hand-computes a pixel. A test of the eye asks per frame (`FrameOptions`).
        options.mProfile.mExposure = 1.0f;
        // And no painted light divided out, so a texture a test hands over is the albedo it traces,
        // which is what its expectation is computed from. A test of the estimate asks per frame.
        options.mProfile.mDelight = 0.0f;

        return options;
    }

    VulkanRenderer& getRenderer()
    {
        return cachedRenderer(true);
    }

    const std::vector<std::string>& getRendererMadeWith()
    {
        return rendererMadeWith();
    }

    VulkanRenderer& getUnvalidatedRenderer()
    {
        return cachedRenderer(false);
    }

    DeviceTest::DeviceTest(bool validation)
        : mHarness(validation ? getHarness() : getUnvalidatedHarness())
    {
    }

    void DeviceTest::SetUp()
    {
        // **Taken and then dropped**, because `takeErrorsOnThisThread` appends where a renderer's
        // own `takeValidationErrors` clears first: the log has to be emptied even though nothing
        // reads what comes off it here.
        takeRaised();
        mRaised.clear();
    }

    void DeviceTest::TearDown()
    {
        takeRaised();
        for (const std::string& error : mRaised)
            ADD_FAILURE() << "validation error: " << error;
    }

    void DeviceTest::takeRaised()
    {
        // Nothing at all where the layers are not loaded, which is the unvalidated device
        // `getUnvalidatedHarness` says why there is.
        if (ValidationLog* log = mHarness.mInstance->getValidationLog(); log != nullptr)
            log->takeErrorsOnThisThread(mRaised);
    }

    CommandPool& DeviceTest::getPool() const
    {
        return getDevice().getPool();
    }

    void RendererTest::SetUp()
    {
        forgetErrors(mRenderer);
    }

    void RendererTest::TearDown()
    {
        reportErrors(mRenderer, "validation error");
    }

    void RendererTest::reportErrors(VulkanRenderer& renderer, std::string_view what)
    {
        renderer.takeValidationErrors(mErrors);
        for (const std::string& error : mErrors)
            ADD_FAILURE() << what << ": " << error;
    }
}
