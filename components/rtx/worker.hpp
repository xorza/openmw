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
    /// A thread this object starts once, stops and joins.
    ///
    /// **Declare it last in whatever owns it.** A member declared after the thread is one the
    /// thread may still be reading while it is destroyed, and the join this begins with is the only
    /// thing between the two. Every owner of one says so at its own declaration and the rule is
    /// this one.
    ///
    /// **What it works on stays the caller's.** A worker is a thread and nothing else: the channel
    /// between it and the frame is `Rtx::Monitor`, and what travels across the channel belongs to
    /// whoever built it.
    ///
    /// **Started by the first thing that asks**, so a world that never reaches distant ground never
    /// pays for a thread.
    class Worker
    {
    public:
        Worker() = default;

        /// Stops and joins whatever is running.
        ~Worker() { stop(); }

        /// Runs `work` on a thread of its own, where none is running.
        ///
        /// **`work` must give up on the stop token it is handed**, which is what `stop` breaks its
        /// wait with. `Monitor::serve` and `repeat` below are the two shapes that do, and a body
        /// that waited on a token it was never passed would never be woken — the join would hang.
        ///
        /// @return whether this call is what started it. **Answered rather than silent**, because a
        /// caller that clears what the last run left has to know it is not clearing a run in
        /// progress.
        bool start(std::function<void(std::stop_token)> work)
        {
            if (mThread.joinable())
                return false;

            mThread = std::jthread(std::move(work));
            return true;
        }

        /// Runs `tick` straight away and every `period` after it, until stopped.
        ///
        /// **A sampler and not a queue.** Nothing ever wakes this, and the wait is a condition
        /// variable only so the stop can break it: `std::this_thread::sleep_for` would hold the
        /// thread for a whole period and make every join wait one out.
        ///
        /// The lock the wait needs is this loop's own, so nothing outside can notify it and nothing
        /// about it is shared with whatever `tick` writes into.
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

    /// Which thread a member belongs to, asserted rather than written down.
    ///
    /// **Every class with a worker has members it calls "the frame thread's own".** That is a
    /// contract the code must keep, which is what an assert is for and not prose. A guard
    /// remembers the thread that built it, and a method that touches what it stands for asks it
    /// first.
    ///
    /// **The check is the assert and nothing else**, so release pays for no comparison. The id
    /// stays a member in both builds rather than hiding behind a gate: it is one word, and a class
    /// that changed shape between builds is a worse trade than that word.
    ///
    /// Untrusted data never reaches one of these. What it guards is which thread is calling, which
    /// is the code's own business and never the content's.
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
