#pragma once

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace Rtx
{
    /// A thread this object starts once, stops and joins. Declare it last in whatever owns it, or
    /// a member declared after it is one the thread may still be reading while it is destroyed. A
    /// thread and nothing else: the channel is `Rtx::Monitor`. Started by the first thing that
    /// asks.
    class Worker
    {
    public:
        Worker() = default;

        /// Stops and joins whatever is running.
        ~Worker() { stop(); }

        /// Runs `work` on a thread of its own, where none is running. `work` must give up on the
        /// stop token it is handed, or the join hangs; `Monitor::serve` and `repeat` are the two
        /// shapes that do.
        ///
        /// @return whether this call is what started it, so a caller that clears what the last run
        ///         left knows it is not clearing a run in progress.
        bool start(std::function<void(std::stop_token)> work)
        {
            if (mThread.joinable())
                return false;

            mThread = std::jthread(std::move(work));
            return true;
        }

        /// Runs `tick` straight away and every `period` after it, until stopped. The wait is a
        /// condition variable only so the stop can break it, where `sleep_for` would make every
        /// join wait a period out.
        ///
        /// @return what `start` answers.
        template <class Tick>
        bool repeat(std::chrono::milliseconds period, Tick tick)
        {
            return start([period, tick = std::move(tick)](std::stop_token stop) {
                std::mutex idle;
                std::condition_variable_any wake;

                for (;;)
                {
                    tick();

                    std::unique_lock<std::mutex> lock(idle);
                    if (wake.wait_for(lock, stop, period, [&stop] { return stop.stop_requested(); }))
                        return;
                }
            });
        }

        /// Stops and joins. Nothing where nothing is running.
        void stop() { mThread = {}; }

    private:
        std::jthread mThread;
    };

    /// Which thread a member belongs to, asserted rather than written down. The check is the
    /// assert and nothing else, so release pays for no comparison; the id stays a member in both
    /// builds, because a class that changed shape between builds is a worse trade than one word.
    class OwnedBy
    {
    public:
        /// Belongs to whoever built it, which for a member is whoever built the class.
        OwnedBy() = default;

        /// Fires where this is not that thread.
        void check() const { assert(mOwner == std::this_thread::get_id() && "a member touched from the wrong thread"); }

        /// Hands it to the calling thread, for the one case the design allows: an owner built on
        /// one thread and driven from another.
        void adopt() { mOwner = std::this_thread::get_id(); }

    private:
        std::thread::id mOwner = std::this_thread::get_id();
    };
}
