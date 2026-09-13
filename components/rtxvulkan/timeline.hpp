#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "owned.hpp"

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
    class Timeline
    {
    public:
        explicit Timeline(const Device& device);

        VkSemaphore getHandle() const { return mHandle.get(); }

        /// Takes the value the next submit signals. The caller signals it, with `signal`.
        std::uint64_t next() { return ++mSubmitted; }

        /// The value the next submit will signal — what a batch recorded now and deferred rides,
        /// because the pool puts every deferred batch ahead of its next submit.
        std::uint64_t getNext() const { return mSubmitted + 1; }

        /// The counter as the queue has advanced it, asked of the device only where the cached
        /// reading is not enough: a value once passed stays passed.
        bool hasFinished(std::uint64_t value) const;

        /// Whether every submit that names `buffer` has run — what a host write of it asserts in a
        /// build that asserts, because a host write over a submit still reading is the one hazard
        /// the layers cannot see.
        bool hasFinished(const Buffer& buffer) const { return hasFinished(buffer.getNamedUntil()); }

        /// The counter as the queue has advanced it, asked of the device now.
        std::uint64_t getFinished() const;

        /// The highest value the device has been seen to pass, asked of nothing: what a wait or a
        /// question left behind. What is retired against, once per wait rather than once per
        /// object.
        std::uint64_t getKnownFinished() const { return mFinished; }

        /// Blocks until the queue has signalled `value`. `what` names the wait in the error a
        /// device that stops answering produces.
        void waitFor(std::uint64_t value, const char* what) const;

        /// The signal a submit puts in its `pSignalSemaphoreInfos` for `value`.
        VkSemaphoreSubmitInfo signal(std::uint64_t value) const;

    private:
        const Device& mDevice;
        Owned<VkSemaphore, vkDestroySemaphore> mHandle;
        std::uint64_t mSubmitted = 0;

        /// The highest value the device has been seen to pass. Mutable because asking is not a
        /// change to the clock, only to what is known of it.
        mutable std::uint64_t mFinished = 0;
    };
}
