#pragma once

#include <functional>
#include <stop_token>
#include <thread>

namespace Rtx
{
    /// A thread this object starts once, stops and joins.
    ///
    /// **The stop and the join both threads in this library wrote out for themselves.** A member
    /// declared after the thread is one the thread may still be reading while it is destroyed, and
    /// a `Worker` declared last is what makes that impossible to get wrong.
    ///
    /// **What it works on stays the caller's.** The two threads have nothing else in common: one
    /// takes a queue of jobs and the other a list that a newer list replaces, so the channel is not
    /// this type's to hold.
    ///
    /// **Started by the first thing that asks**, so a world that never reaches distant ground never
    /// pays for a thread.
    class Worker
    {
    public:
        Worker() = default;

        /// Stops and joins whatever is running.
        ~Worker() { stop(); }

        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;

        /// Runs `work` on a thread of its own, where none is running.
        ///
        /// **`work` must wait on the stop token it is handed**, which is what `stop` breaks the wait
        /// with — `std::condition_variable_any::wait(lock, stop, pred)` is the form both callers
        /// take. One that waited on a token it was never passed would never be woken, and the join
        /// below would hang.
        void start(std::function<void(std::stop_token)> work)
        {
            if (mThread.joinable())
                return;

            mThread = std::jthread(std::move(work));
        }

        /// Stops and joins. Nothing where nothing is running.
        void stop() { mThread = {}; }

    private:
        std::jthread mThread;
    };
}
