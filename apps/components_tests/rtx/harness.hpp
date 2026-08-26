#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <components/rtx/error.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxvulkan/device.hpp>
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

    namespace Details
    {
        /// Something built once for the whole binary, and the reason where it could not be.
        ///
        /// Held here rather than in a function-local static so that `releaseDevices` can close it
        /// while the process is still whole; see the environment in `harness.cpp`.
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

            /// Closes it, and says so to anything that asks afterwards rather than answering an
            /// empty reason — which a test would report as a skip with no explanation.
            void release()
            {
                mValue.reset();
                mReason = "the suite closed its devices after the last test";
            }
        };

        /// Keyed on validation, which is the only axis any of these vary along.
        inline Once<Harness>& harnessCache(bool validation)
        {
            static Once<Harness> sValidated;
            static Once<Harness> sPlain;
            return validation ? sValidated : sPlain;
        }

        inline Once<Renderer>& rendererCache(bool validation)
        {
            static Once<Renderer> sValidated;
            static Once<Renderer> sPlain;
            return validation ? sValidated : sPlain;
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
    /// One test wants this and wants it for a particular reason: the layers go to the heap on every
    /// command they inspect — sixty-six times a frame in this renderer — which drowns out anything
    /// an allocation count is trying to see. Everything else is better off validated, so this second
    /// device is only built if something asks for it.
    inline Harness* getUnvalidatedHarness(std::string& reason)
    {
        return Details::harnessCache(false).get(reason, [](std::string& why) { return Details::build(false, why); });
    }

    /// Where the build wrote the compiled shaders.
    inline std::filesystem::path getShaderDirectory()
    {
        return std::filesystem::path(OPENMW_RTX_SHADER_DIR);
    }

    namespace Details
    {
        inline std::unique_ptr<Renderer> buildRenderer(bool validation, std::string& reason)
        {
            RendererOptions options;
            options.mShaderDirectory = getShaderDirectory();
            // Every test resizes to what it needs; this is only what the first target costs.
            options.mWidth = 1;
            options.mHeight = 1;
            options.mValidation.mEnabled = validation;
            // Tests provoke errors deliberately and assert on them; aborting would take the suite
            // down with the first one.
            options.mValidation.mAbortOnError = false;
            // **On, because a missing barrier is what this suite is worst at seeing.** Every test
            // here submits and waits, so the ordering a frame relies on is supplied by the harness
            // rather than by the code under test, and a hazard shows as nothing at all — a traced
            // view wrote its picture with no dependency on the write before it for as long as there
            // have been traced views. It costs no measurable time in this suite.
            options.mValidation.mSynchronization = true;

            return createRenderer(options, reason);
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
    inline Renderer* getRenderer(std::string& reason)
    {
        return Details::rendererCache(true).get(
            reason, [](std::string& why) { return Details::buildRenderer(true, why); });
    }

    /// The same, uninstrumented, for the one test that counts allocations.
    ///
    /// The layers go to the heap on every command they inspect, which drowns out anything an
    /// allocation count is trying to see.
    inline Renderer* getUnvalidatedRenderer(std::string& reason)
    {
        return Details::rendererCache(false).get(
            reason, [](std::string& why) { return Details::buildRenderer(false, why); });
    }

}
