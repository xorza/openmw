#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/error.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/requirements.hpp>

namespace Rtx::Testing
{
    /// Instance and device for the whole test binary.
    ///
    /// Bring-up costs a good fraction of a second and nothing mutates it, so paying once is the
    /// difference between a suite that gets run and one that does not. Both are held by pointer so
    /// the instance can be built and inspected before there is a device to pair it with, and so it
    /// is destroyed last.
    struct Harness
    {
        std::unique_ptr<Instance> mInstance;
        std::unique_ptr<Device> mDevice;
    };

    /// Why this machine cannot build a Vulkan instance, or empty where it can.
    ///
    /// The two ways a machine legitimately has nothing to trace with: no loader, or a loader with no
    /// driver behind it — which fails at `vkCreateInstance` with `VK_ERROR_INCOMPATIBLE_DRIVER`
    /// rather than by handing back an empty device list. Both are a skip.
    ///
    /// Shared with the tests that build an instance of their own so the two cannot come to disagree
    /// about which failure is honest and which is a finding. Whether a device that *does* exist
    /// qualifies is a different question, and `PhysicalDevice::select` still throws it.
    inline std::string findInstanceObstacle()
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

    /// Something built once for the whole binary, and the reason where it could not be.
    ///
    /// **Held by whoever declares one rather than in a function-local static**, so that a gtest
    /// environment can close it while the process is still whole — see the teardowns in
    /// `harness.cpp` and `rtxtool/installation.cpp`, and the comment on `DeviceTeardown` for what
    /// static destruction did instead.
    template <class T>
    struct Once
    {
        std::unique_ptr<T> mValue;
        std::string mReason;
        bool mTried = false;

        template <class Build>
        T* get(std::string& reason, Build&& build)
        {
            if (!mTried)
            {
                mTried = true;
                mValue = build(mReason);
            }

            reason = mReason;
            return mValue.get();
        }

        /// Closes it, and says so to anything that asks afterwards rather than answering an empty
        /// reason — which a test would report as a skip with no explanation.
        void release(std::string why)
        {
            mValue.reset();
            mReason = std::move(why);
        }
    };

    namespace Details
    {
        /// Keyed on validation, which is the only axis any of these vary along.
        inline Once<Harness>& harnessCache(bool validation)
        {
            static Once<Harness> sValidated;
            static Once<Harness> sPlain;
            return validation ? sValidated : sPlain;
        }

        inline Once<Renderer>& rendererCache()
        {
            static Once<Renderer> sRenderer;
            return sRenderer;
        }

        inline std::unique_ptr<Harness> build(bool validation, std::string& reason)
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
                *harness->mInstance, PhysicalDevice::select(harness->mInstance->getHandle()));
            return harness;
        }
    }

    /// Null when this machine has no Vulkan device at all, with `reason` saying so.
    ///
    /// A machine without a GPU legitimately cannot run these, and skipping is honest. A machine
    /// *with* one that does not meet the requirements is a finding, so that throws out of
    /// `PhysicalDevice::select` and fails the suite rather than skipping.
    inline Harness* getHarness(std::string& reason)
    {
        return Details::harnessCache(true).get(reason, [](std::string& why) { return Details::build(true, why); });
    }

    /// The same, with no validation layers loaded.
    ///
    /// **The one thing in this suite that runs unvalidated, and it is measured rather than
    /// asserted.** `getAllocationCount` replaces the global `operator new`, so it cannot tell a
    /// layer's allocation from the renderer's; with the layers loaded, `RtxFrameCostTest` measures
    /// 28,448 allocations over its 32 frames — 889 a frame against a budget of nought. That is not
    /// a stricter test but a deleted one. Everything else is validated, so this second device is
    /// only built if something asks for it.
    inline Harness* getUnvalidatedHarness(std::string& reason)
    {
        return Details::harnessCache(false).get(reason, [](std::string& why) { return Details::build(false, why); });
    }

    /// Where the build wrote the compiled shaders.
    inline std::filesystem::path getShaderDirectory()
    {
        return std::filesystem::path(OPENMW_RTX_SHADER_DIR);
    }

    /// How every renderer in this suite is built, apart from the extent and the upscaler a caller
    /// sets for itself.
    ///
    /// **One place, so that a test standing up its own renderer cannot end up validated less than
    /// the shared one.**
    inline RendererOptions describeRenderer(std::uint32_t width, std::uint32_t height)
    {
        RendererOptions options;
        options.mShaderDirectory = getShaderDirectory();
        options.mWidth = width;
        options.mHeight = height;
        options.mValidation.mEnabled = true;
        // Tests provoke errors deliberately and assert on them; aborting would take the suite down
        // with the first one.
        options.mValidation.mAbortOnError = false;
        // **On, because a missing barrier is what this suite is worst at seeing.** Every test here
        // submits and waits, so the ordering a frame relies on is supplied by the harness rather
        // than by the code under test, and a hazard shows as nothing at all — a traced view wrote
        // its picture with no dependency on the write before it for as long as there have been
        // traced views. It costs no measurable time in this suite.
        options.mValidation.mSynchronization = true;

        return options;
    }

    namespace Details
    {
        inline std::unique_ptr<Renderer> buildRenderer(std::string& reason)
        {
            // Every test resizes to what it needs; one texel is only what the first target costs.
            return createRenderer(describeRenderer(1, 1), reason);
        }
    }

    /// The renderer the pixel tests trace through, built once for the binary.
    ///
    /// Null with `reason` where this machine cannot run the backend this build has — which is the
    /// ordinary case on a box developing the other one, and a skip rather than a failure.
    ///
    /// **What makes these tests an acceptance suite for any backend.** They assert hand-computed
    /// radiances, mip levels and transmittances, none of which is a statement about an API; a
    /// backend that passes this file is correct.
    ///
    /// **One for the binary, and a second is what a slow test is made of.** Measured with the
    /// on-disk pipeline cache warm: `createRenderer` costs 700–870 ms, the first `setScene` on the
    /// result costs another 900–1150 ms because it compiles every kernel the trace can need — see
    /// `VulkanRenderer::setScene` — and every `resize`, `setScene`, `renderFrame` and `readPixels`
    /// after that costs between one and fifteen. So a test that traces through this one is free and
    /// a test that stands up its own costs the suite two seconds. Only an upscaler needs its own,
    /// because the mode is fixed when the renderer is built.
    inline Renderer* getRenderer(std::string& reason)
    {
        return Details::rendererCache().get(reason, Details::buildRenderer);
    }

    /// The base of a test that drives Vulkan directly.
    ///
    /// **One shape for the skip.** The suite had two: a fixture in some files and the same four
    /// lines written out in every test of the others. Which one a file used said nothing about the
    /// file, and a test that has to remember to ask for the reason is a test that can forget to.
    class DeviceTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            std::string reason;
            mHarness = getHarness(reason);
            if (mHarness == nullptr)
                GTEST_SKIP() << reason;
        }

        Device& getDevice() const { return *mHarness->mDevice; }

        /// A pool on that device, opened on the first ask and closed with the test.
        CommandPool& getPool()
        {
            if (mPool == nullptr)
                mPool = std::make_unique<CommandPool>(getDevice());

            return *mPool;
        }

        Harness* mHarness = nullptr;

    private:
        std::unique_ptr<CommandPool> mPool;
    };

    /// The base of a test that renders.
    ///
    /// **The validation errors are drained before the test and reported after it**, which two of the
    /// five files that render did not do at all: a hazard the layers caught went onto a list nothing
    /// ever read. Draining first is how the slate is cleared — whatever a previous test left behind
    /// is not this one's to report.
    class RendererTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            std::string reason;
            mRenderer = getRenderer(reason);
            if (mRenderer == nullptr)
                GTEST_SKIP() << reason;

            mRenderer->takeValidationErrors(mErrors);
        }

        void TearDown() override
        {
            if (mRenderer == nullptr)
                return;

            mRenderer->takeValidationErrors(mErrors);
            for (const std::string& error : mErrors)
                ADD_FAILURE() << "validation error: " << error;
        }

        Renderer* mRenderer = nullptr;

    private:
        std::vector<std::string> mErrors;
    };

    /// One half float, as the number it stands for.
    ///
    /// **Spelled out rather than shared with the renderer, and by arithmetic rather than by bits.**
    /// Several passes keep their output in halves, so a test that read them through the same helper
    /// the shader used would pass however wrong that helper was — and one written in shifts and
    /// masks is a second place for the subnormal case to be wrong.
    inline float fromHalf(std::uint16_t bits)
    {
        const float sign = (bits & 0x8000u) != 0 ? -1.0f : 1.0f;
        const int exponent = (bits >> 10) & 0x1f;
        const int mantissa = bits & 0x3ff;

        if (exponent == 0)
            return sign * std::ldexp(static_cast<float>(mantissa), -24);

        if (exponent == 31)
            return sign
                * (mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN());

        return sign * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, exponent - 15);
    }

    /// Every channel of one level of a half-float image, decoded, row major.
    ///
    /// **Left in the layout it was found in**, which `Image::read` promises: reading an image is not
    /// a change to it. Several passes keep their output in halves, so this is the read-back beside
    /// the decoder rather than one copy of it per suite.
    inline std::vector<float> readHalves(CommandPool& pool, const Image& image, std::uint32_t level = 0)
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
