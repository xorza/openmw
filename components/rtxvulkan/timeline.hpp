#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "handles.hpp"

namespace Rtx
{
    class Device;

    /// The queue's one clock: a timeline semaphore that every submit the pool makes signals with
    /// the next value, so "has that finished" is a comparison against a counter the queue advances
    /// and never a fence somebody has to own, reset and remember. A resource retires against the
    /// value of the submit that last named it, a question recorded into a batch is readable once
    /// the value the batch rode has passed, and a frame is the value its trace signalled.
    ///
    /// **A count of frames, placements or calls never stands in for it.** The store counted
    /// placements and called that a fence; two placements in one frame made the count run ahead of
    /// the queue.
    ///
    /// **And the clock is never read off the device on the frame path.** What the host knows the
    /// queue has passed is what a wait left behind, and the frame ring waits once a frame: so
    /// what a frame reads of the clock is a function of how many frames were drawn, and never of
    /// how fast the device drew them. Asked of the device, the answer moved with the wall — a
    /// sprite list was sized from a report that had or had not landed, a structure was compacted a
    /// frame earlier or later, a buried buffer was freed and its room reused a frame sooner — and
    /// two builds of one tree drew three pixels apart. `repeat` could not see it, because two runs
    /// of one binary keep the same phase.
    ///
    /// **Every wait is the device's**, `Device::waitFor` or `Device::waitIdle`, and nothing else
    /// waits: a wait is where what the clock knows changes, so it is where what the queue may
    /// still read changes, and the device is what collects that — a caller that waited here and
    /// forgot to collect would hold what it could have freed.
    class Timeline
    {
    public:
        explicit Timeline(const Device& device);

        VkSemaphore getHandle() const { return mHandle.get(); }

        /// The value the next submit will signal — what a batch recorded now and deferred rides,
        /// because the pool puts every deferred batch ahead of its next submit.
        std::uint64_t getNext() const { return mSubmitted + 1; }

        /// Whether the host has waited past `value`: a comparison against what a wait left behind,
        /// and never a question to the device. A value once passed stays passed.
        bool hasFinished(const std::uint64_t value) const { return value <= mFinished; }

        /// Whether the host has waited past every submit made: nothing is on the queue.
        bool isIdle() const { return mFinished == mSubmitted; }

        /// The highest value a wait has left behind. What is retired against, once per wait rather
        /// than once per object.
        std::uint64_t getKnownFinished() const { return mFinished; }

        /// The signal a submit puts in its `pSignalSemaphoreInfos` for `value`.
        VkSemaphoreSubmitInfo signal(std::uint64_t value) const;

    private:
        friend class CommandPool;
        friend class Device;

        /// Takes the value the next submit signals, which the pool signals with `signal`. The
        /// pool's alone: a second caller would put the clock ahead of the queue.
        std::uint64_t next() { return ++mSubmitted; }

        /// Blocks until the queue has signalled `value`. `what` names the wait in the error a
        /// device that stops answering produces. The device's alone, for the reason the class
        /// doc gives.
        void waitFor(std::uint64_t value, const char* what) const;

        /// What a device idle leaves behind: every submit made has run. The device says so after
        /// `vkDeviceWaitIdle`, which is a wait the semaphore is not asked about.
        void markIdle() const;

        const Device& mDevice;
        Semaphore mHandle;
        std::uint64_t mSubmitted = 0;

        /// The highest value a wait has left behind. Mutable because waiting is not a change to
        /// the clock, only to what is known of it.
        mutable std::uint64_t mFinished = 0;
    };
}
