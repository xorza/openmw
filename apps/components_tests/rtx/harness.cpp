#include "harness.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include <gtest/gtest.h>

#include <components/files/configurationmanager.hpp>
#include <components/rtx/error.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/requirements.hpp>

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
        Once<Renderer>& rendererCache(bool validation)
        {
            static Once<Renderer> sValidated;
            static Once<Renderer> sPlain;
            return validation ? sValidated : sPlain;
        }

        std::unique_ptr<Harness> build(bool validation, std::string& reason)
        {
            if (std::string obstacle = findInstanceObstacle(); !obstacle.empty())
            {
                reason = std::move(obstacle);
                return nullptr;
            }

            InstanceOptions options;
            options.mValidation = validation;
            // Tests provoke errors deliberately and assert on them; aborting would take the suite
            // down with the first one.
            options.mPolicy = ValidationPolicy::Log;

            auto harness = std::make_unique<Harness>();
            harness->mInstance = std::make_unique<Instance>(options);

            std::uint32_t count = 0;
            if (vkEnumeratePhysicalDevices(harness->mInstance->getHandle(), &count, nullptr) != VK_SUCCESS
                || count == 0)
            {
                reason = "no Vulkan device is installed";
                return nullptr;
            }

            harness->mDevice = std::make_unique<Device>(
                *harness->mInstance, PhysicalDevice::select(harness->mInstance->getHandle()), getPipelineCacheSpec());
            return harness;
        }

        /// **The flag reaches the cache and the build together**, so the two cannot come apart: a
        /// device built unvalidated and filed under the validated key would be handed to every test
        /// in the suite.
        Harness* cachedHarness(bool validation, std::string& reason)
        {
            return harnessCache(validation).get(reason, [validation](std::string& why) {
                return build(validation, why);
            });
        }

        std::unique_ptr<Renderer> buildRenderer(bool validation, std::string& reason)
        {
            // Every test resizes to what it needs; one texel is only what the first target costs.
            return createRenderer(describeRenderer(1, 1, validation), reason);
        }

        Renderer* cachedRenderer(bool validation, std::string& reason)
        {
            return rendererCache(validation).get(reason, [validation](std::string& why) {
                return buildRenderer(validation, why);
            });
        }

        /// One half float, as the number it stands for.
        ///
        /// **Spelled out rather than shared with the renderer, and by arithmetic rather than by
        /// bits.** Several passes keep their output in halves, so a test that read them through the
        /// same helper the shader used would pass however wrong that helper was — and one written in
        /// shifts and masks is a second place for the subnormal case to be wrong.
        float fromHalf(std::uint16_t bits)
        {
            const float sign = (bits & 0x8000u) != 0 ? -1.0f : 1.0f;
            const int exponent = (bits >> 10) & 0x1f;
            const int mantissa = bits & 0x3ff;

            if (exponent == 0)
                return sign * std::ldexp(static_cast<float>(mantissa), -24);

            if (exponent == 31)
                return sign
                    * (mantissa == 0 ? std::numeric_limits<float>::infinity()
                                     : std::numeric_limits<float>::quiet_NaN());

            return sign * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, exponent - 15);
        }

        /// Closes every device the binary cached, after the last test and before `main` returns.
        ///
        /// **Two Vulkan devices destroyed after `main` has returned abort inside the validation
        /// layer**, with no message and no stack of ours on it. One pair survives static destruction
        /// and a second does not — reproduced with nothing in the process but two instances left to
        /// exit — and this binary keeps up to four: a raw device for the tests that drive Vulkan
        /// directly and a `Renderer` for the pixel suite, each in a validated and an unvalidated
        /// flavour. Closing them here is both the fix and where they belonged: a cache that lives
        /// for the run should end with the run, not with the process.
        class DeviceTeardown : public ::testing::Environment
        {
            void TearDown() override
            {
                const std::string why = "the suite closed its devices after the last test";

                // The renderer before the raw devices, which is the order they were built in; neither
                // depends on the other.
                for (const bool validation : { true, false })
                    rendererCache(validation).release(why);
                for (const bool validation : { true, false })
                    harnessCache(validation).release(why);
            }
        };

        // Before `main`, because gtest only tears down environments registered before the run starts.
        [[maybe_unused]] const bool sRegistered = [] {
            ::testing::AddGlobalTestEnvironment(new DeviceTeardown);
            return true;
        }();
    }

    std::string findInstanceObstacle()
    {
        std::uint32_t version = 0;
        if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < sApiVersion)
            return "the Vulkan loader is absent or older than this renderer requires";

        try
        {
            const Instance probe{ InstanceOptions{} };
        }
        catch (const Error& error)
        {
            return std::string("no Vulkan driver is installed: ") + error.what();
        }

        return {};
    }

    Harness* getHarness(std::string& reason)
    {
        return cachedHarness(true, reason);
    }

    Harness* getUnvalidatedHarness(std::string& reason)
    {
        return cachedHarness(false, reason);
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
        options.mValidation.mEnabled = validation;
        // Tests provoke errors deliberately and assert on them; aborting would take the suite down
        // with the first one.
        options.mValidation.mAbortOnError = false;
        // **On wherever the layers are, because a missing barrier is what this suite is worst at
        // seeing.** Every test here submits and waits, so the ordering a frame relies on is supplied
        // by the harness rather than by the code under test, and a hazard shows as nothing at all —
        // a traced view wrote its picture with no dependency on the write before it for as long as
        // there have been traced views. It costs no measurable time in this suite.
        options.mValidation.mSynchronization = validation;

        return options;
    }

    Renderer* getRenderer(std::string& reason)
    {
        return cachedRenderer(true, reason);
    }

    Renderer* getUnvalidatedRenderer(std::string& reason)
    {
        return cachedRenderer(false, reason);
    }

    DeviceTest::DeviceTest(bool validation)
        : mValidation(validation)
    {
    }

    void DeviceTest::SetUp()
    {
        std::string reason;
        mHarness = mValidation ? getHarness(reason) : getUnvalidatedHarness(reason);
        if (mHarness == nullptr)
            GTEST_SKIP() << reason;
    }

    CommandPool& DeviceTest::getPool()
    {
        if (mPool == nullptr)
            mPool = std::make_unique<CommandPool>(getDevice());

        return *mPool;
    }

    void RendererTest::SetUp()
    {
        std::string reason;
        mRenderer = getRenderer(reason);
        if (mRenderer == nullptr)
            GTEST_SKIP() << reason;

        mRenderer->takeValidationErrors(mErrors);
    }

    void RendererTest::TearDown()
    {
        if (mRenderer == nullptr)
            return;

        mRenderer->takeValidationErrors(mErrors);
        for (const std::string& error : mErrors)
            ADD_FAILURE() << "validation error: " << error;
    }

    std::vector<float> readHalves(CommandPool& pool, const Image& image, std::uint32_t level)
    {
        std::vector<std::uint8_t> bytes;
        image.read(pool, VK_IMAGE_LAYOUT_GENERAL, bytes, level);

        std::vector<float> values(bytes.size() / sizeof(std::uint16_t));
        for (std::size_t at = 0; at < values.size(); ++at)
        {
            std::uint16_t bits = 0;
            std::memcpy(&bits, bytes.data() + at * sizeof(bits), sizeof(bits));
            values[at] = fromHalf(bits);
        }

        return values;
    }
}
