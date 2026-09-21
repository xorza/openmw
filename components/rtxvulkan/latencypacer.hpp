#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <vulkan/vulkan_core.h>

#include <components/rtx/latencyreport.hpp>
#include <components/rtx/pacing.hpp>

#include "device.hpp"

namespace Rtx
{
    /// The driver's frame pacing over one swapchain — Reflex, `VK_NV_low_latency2`. One sleep
    /// before each present, at the top of the frame where input is about to be read, which the
    /// driver lengthens until the frame's input is sampled as late as it will still reach the
    /// screen on time; the markers that tell the driver where the frame's simulation, submission
    /// and present begin and end; and the id every present and every submit carries so the
    /// driver can tell one frame's markers from the next's.
    ///
    /// **Every present is paced where the driver paces at all.** The mode is a flag inside the
    /// sleep mode and the frame-rate limit is the interval the sleep enforces, so an application
    /// that turned the calls off with the mode would sleep at a worse spot, in its own limiter,
    /// after the present. Dormant — every call nothing — where the device or the surface paces
    /// nothing, which is the same shape as no extension.
    ///
    /// **A frame is one sleep to one present, and the turns say where it stands.** The driver
    /// counts one sleep between two presents; a window hidden, or a present that never reached
    /// the call, leaves the frame open, and the next sleep is refused until a present closes it. A
    /// present with no sleep before it — a loading screen's, a message box's — pays the sleep it
    /// owes before its acquire, at a worse spot, so the driver's count holds.
    ///
    /// **The sleep's wait is not the queue's clock.** `Timeline` says every wait is the device's,
    /// because a wait is where what the queue has passed changes; nothing on the queue signals
    /// this semaphore, so waiting on it changes nothing about what may be freed.
    ///
    /// Made from the device's entry points and not from the device, so a test can stand in for
    /// every call: what this class decides — the turns, the ids, the markers' order — is host
    /// logic, and the tests that reach it have no device. The price is that a device lost inside
    /// the sleep's wait is reported without the fault description a queue wait would carry; the
    /// next queue wait carries it.
    class LatencyPacer
    {
    public:
        /// @param functions the device's, every one null where the driver paces nothing.
        /// @param sleepSemaphore a timeline semaphore the sleep signals, the caller's for as long
        ///        as this lives.
        LatencyPacer(const LatencyFunctions& functions, VkDevice device, VkSemaphore sleepSemaphore);

        /// Points this at a swapchain just made, and says whether the surface paces the mode it
        /// was made with: live from here where it does and the driver takes the mode, dormant
        /// where either is false. Applies the sleep mode, which is the swapchain's and does not
        /// survive a rebuild. A frame open across a rebuild stays open — the resize happened
        /// inside it — and a pacer that went dormant holds no frame, so it opens the next.
        void follow(VkSwapchainKHR swapchain, bool pacedMode);

        bool isLive() const { return mLive; }

        /// The mode and the interval, applied at once where live and kept for the next `follow`.
        void setPacing(const Pacing& pacing);

        /// The sleep and its wait, then `SIMULATION_START` and `INPUT_SAMPLE`: opens the frame.
        /// Nothing where dormant, and nothing on a frame still open.
        void awaitFrame();

        /// `SIMULATION_END` and `RENDERSUBMIT_START`, with `TRIGGER_FLASH` before them where
        /// `flash`. Nothing where dormant, nothing where no frame is open, nothing twice.
        void endSimulation(bool flash);

        /// Just before `vkQueuePresentKHR`: pays a sleep still owed, closes a simulation still
        /// open, then `RENDERSUBMIT_END` and `PRESENT_START`. `afterPresent` must follow.
        void beforePresent();

        /// Just after the call, whatever it answered: `PRESENT_END`, the timings read, the frame
        /// closed and the next sleep owed.
        void afterPresent();

        /// The id the open frame carries, or the one the next frame will open with — what every
        /// submit between two presents and the present itself are stamped with. Nought where
        /// dormant, which chains nothing.
        std::uint64_t getPresentId() const;

        /// The newest frame the driver has timings for, or nothing.
        std::optional<LatencyReport> describeLatency() const { return mLatest; }

    private:
        /// Where the frame stands between one sleep and the next: owing its sleep, slept and
        /// simulating, submitting, or inside the present call.
        enum class Turn
        {
            Owed,
            Slept,
            Submitting,
            Presenting,
        };

        void applyMode();
        void mark(VkLatencyMarkerNV marker);
        void readTimings();

        const LatencyFunctions& mFunctions;
        VkDevice mDevice;
        VkSemaphore mSleepSemaphore;
        VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;

        bool mLive = false;
        bool mRefused = false;
        Pacing mPacing;
        Turn mTurn = Turn::Owed;

        /// Strictly increasing for the life of the pacer, across every swapchain it follows: a
        /// number that went back is a frame the driver cannot place.
        std::uint64_t mPresentId = 0;
        std::uint64_t mSleepCounter = 0;

        /// The driver's ring, as `vkGetLatencyTimingsNV` fills it: sized once to what NvAPI keeps,
        /// so a frame allocates nothing to read it.
        static constexpr std::size_t sTimingsKept = 64;
        std::array<VkLatencyTimingsFrameReportNV, sTimingsKept> mTimings{};
        std::optional<LatencyReport> mLatest;
    };
}
