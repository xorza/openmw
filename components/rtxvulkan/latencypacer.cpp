#include "latencypacer.hpp"

#include <cassert>
#include <cstddef>

#include <components/debug/debuglog.hpp>

#include "result.hpp"

namespace Rtx
{
    LatencyPacer::LatencyPacer(
        const LatencyFunctions& functions, const VkDevice device, const VkSemaphore sleepSemaphore)
        : mFunctions(functions)
        , mDevice(device)
        , mSleepSemaphore(sleepSemaphore)
    {
    }

    void LatencyPacer::follow(const VkSwapchainKHR swapchain, const bool pacedMode)
    {
        mSwapchain = swapchain;
        mLive = pacedMode && mFunctions.mSleep != nullptr && swapchain != VK_NULL_HANDLE;

        // A pacer that paces nothing holds no frame: what it was told before it went dormant
        // closed nowhere the driver could see, so the next sleep opens afresh.
        if (!mLive)
        {
            mTurn = Turn::Owed;
            return;
        }

        applyMode();
    }

    void LatencyPacer::setPacing(const Pacing& pacing)
    {
        if (pacing == mPacing)
            return;

        mPacing = pacing;
        if (mLive)
            applyMode();
    }

    void LatencyPacer::applyMode()
    {
        const VkLatencySleepModeInfoNV mode{
            .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
            .lowLatencyMode = mPacing.mMode != LatencyMode::Off ? VK_TRUE : VK_FALSE,
            .lowLatencyBoost = mPacing.mMode == LatencyMode::Boost ? VK_TRUE : VK_FALSE,
            .minimumIntervalUs = mPacing.mMinimumIntervalUs,
        };

        // A refusal is the driver saying this surface is not paced after all — a dormant pacer
        // and a frame that still presents, said once, rather than a throw out of a resize.
        const VkResult result = mFunctions.mSetSleepMode(mDevice, mSwapchain, &mode);
        if (result == VK_SUCCESS)
            return;

        mLive = false;
        mTurn = Turn::Owed;
        if (!mRefused)
            Log(Debug::Warning) << "Frame pacing: the driver refused the sleep mode with " << resultName(result)
                                << ", so the host paces its own frames";
        mRefused = true;
    }

    void LatencyPacer::mark(const VkLatencyMarkerNV marker)
    {
        const VkSetLatencyMarkerInfoNV info{
            .sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
            .presentID = mPresentId,
            .marker = marker,
        };
        mFunctions.mSetMarker(mDevice, mSwapchain, &info);
    }

    void LatencyPacer::awaitFrame()
    {
        if (!mLive || mTurn != Turn::Owed)
            return;

        ++mPresentId;

        const VkLatencySleepInfoNV sleep{
            .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
            .signalSemaphore = mSleepSemaphore,
            .value = ++mSleepCounter,
        };
        checkVk(mFunctions.mSleep(mDevice, mSwapchain, &sleep), "vkLatencySleepNV");

        // Bounded as every wait here is, because a driver that stops signalling is a frame loop
        // that stops for ever.
        const VkSemaphoreWaitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &mSleepSemaphore,
            .pValues = &sleep.value,
        };
        const VkResult waited = mFunctions.mWaitSemaphores(mDevice, &wait, sPatience);
        if (waited == VK_TIMEOUT)
            deviceFailed(timedOut("the driver's sleep releasing the frame", sPatience));
        checkVk(waited, "vkWaitSemaphores on the frame pacing's semaphore");

        mark(VK_LATENCY_MARKER_SIMULATION_START_NV);
        mark(VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
        mTurn = Turn::Slept;
    }

    void LatencyPacer::endSimulation(const bool flash)
    {
        if (!mLive || mTurn != Turn::Slept)
            return;

        if (flash)
            mark(VK_LATENCY_MARKER_TRIGGER_FLASH_NV);
        mark(VK_LATENCY_MARKER_SIMULATION_END_NV);
        mark(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
        mTurn = Turn::Submitting;
    }

    void LatencyPacer::beforePresent()
    {
        if (!mLive)
            return;

        assert(mTurn != Turn::Presenting && "a present inside a present");

        // A present nothing slept for, and a present nothing ended the simulation for: paid
        // here, so the driver sees the frame it counts, at the spot it happens to be.
        awaitFrame();
        endSimulation(false);

        mark(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
        mark(VK_LATENCY_MARKER_PRESENT_START_NV);
        mTurn = Turn::Presenting;
    }

    void LatencyPacer::afterPresent()
    {
        if (!mLive)
            return;

        assert(mTurn == Turn::Presenting && "a present's end with no present before it");

        mark(VK_LATENCY_MARKER_PRESENT_END_NV);
        mTurn = Turn::Owed;

        readTimings();
    }

    std::uint64_t LatencyPacer::getPresentId() const
    {
        if (!mLive)
            return 0;

        return mTurn == Turn::Owed ? mPresentId + 1 : mPresentId;
    }

    void LatencyPacer::readTimings()
    {
        // Stamped before every read and not once: the driver writes the entries whole, and the
        // layers check every entry's type on the way in, filled or not.
        for (VkLatencyTimingsFrameReportNV& timing : mTimings)
            timing.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;

        VkGetLatencyMarkerInfoNV info{
            .sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV,
            .timingCount = static_cast<std::uint32_t>(mTimings.size()),
            .pTimings = mTimings.data(),
        };
        mFunctions.mGetTimings(mDevice, mSwapchain, &info);

        // The newest is the one with the largest id, wherever the driver put it: the ring's order
        // is the driver's and not promised.
        const VkLatencyTimingsFrameReportNV* newest = nullptr;
        for (std::size_t at = 0; at < info.timingCount; ++at)
        {
            const VkLatencyTimingsFrameReportNV& timing = mTimings[at];
            if (timing.presentID == 0 || timing.presentEndTimeUs == 0)
                continue;
            if (newest == nullptr || timing.presentID > newest->presentID)
                newest = &timing;
        }

        if (newest == nullptr)
            return;

        // A stretch whose start the driver never saw reads as nought and not as a difference of
        // two unrelated numbers.
        const auto stretch = [](const std::uint64_t from, const std::uint64_t to) -> std::uint64_t {
            return from == 0 || to < from ? 0 : to - from;
        };

        mLatest = LatencyReport{
            .mPresentId = newest->presentID,
            .mInputToPresentUs = stretch(newest->inputSampleTimeUs, newest->presentEndTimeUs),
        };
    }
}
