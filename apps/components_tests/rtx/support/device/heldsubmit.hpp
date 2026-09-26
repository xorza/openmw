#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/handles.hpp>

namespace Rtx::Testing
{
    /// A submit the queue cannot start until the host says so: it waits on a timeline semaphore
    /// of its own that only `release` signals.
    ///
    /// **What makes "a submit still on the queue" a state a test can stand in.** A copy or a
    /// dispatch is finished before the host has asked whether it is, so a test of what a host write
    /// must wait for would otherwise be racing a device that always wins. Held, the submit is on
    /// the queue for exactly as long as the test wants it there.
    class HeldSubmit
    {
    public:
        explicit HeldSubmit(const Device& device);

        /// Lets the queue start it on the way out, so a test that fails behind the hold does not
        /// leave the pool's teardown waiting for a submit that can never run.
        ~HeldSubmit();

        HeldSubmit(const HeldSubmit&) = delete;
        HeldSubmit& operator=(const HeldSubmit&) = delete;

        /// Submits `commands`, begun through the device's pool, behind the hold, with whatever the
        /// pool has deferred ahead of it. Ends `commands`. Returns the value the submit signals on
        /// the timeline.
        std::uint64_t submit(VkCommandBuffer commands);

        void release();

        /// Lets it start `delay` from now, from a thread of its own, so a test can stand inside a
        /// wait while the hold opens under it. A wait that returns sooner did not wait, which is
        /// what a bound of `delay` on it says; the thread is joined with the hold.
        void releaseAfter(std::chrono::milliseconds delay);

    private:
        const Device& mDevice;
        Semaphore mGate;
        std::thread mOpener;
        bool mReleased = false;
    };
}
