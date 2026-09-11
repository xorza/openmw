#pragma once

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stop_token>
#include <utility>

namespace Rtx
{
    /// The lock between a frame and the workers it keeps, and the two waits across it.
    ///
    /// **The dance and not the data.** What a worker is handed and what it hands back differ from
    /// one worker to the next — the cell reader takes a list that a newer list replaces, the
    /// bakers take a queue and give back in sequence — so the containers stay with their owner.
    /// What is the same for all of them is a mutex, a condition queue each way, and the rule that a
    /// wait on the worker's side must be one a stop can break. That was written out twice, in two
    /// spellings, and this is the one place it is written.
    ///
    /// **Every operation is a template on what it runs**, so nothing here allocates and nothing
    /// calls through a pointer. A `std::function` on the frame's path would be an allocation per
    /// hand-over.
    ///
    /// **A worker that throws closes the monitor rather than ending the process.** An exception out
    /// of a `std::jthread`'s body is a `std::terminate` that names nothing. The first one is kept,
    /// every waiter is woken, and the frame gets it from `rethrowFailure` at a point where it can
    /// say what happened.
    ///
    /// **Not a queue and not a thread pool.** `Rtx::Worker` owns the thread and the caller owns the
    /// work.
    class Monitor
    {
    public:
        Monitor() = default;

        Monitor(const Monitor&) = delete;
        Monitor& operator=(const Monitor&) = delete;

        /// Runs `write` under the lock and wakes nobody, answering with whatever it answered.
        ///
        /// **By value**, so a caller cannot hand back a reference into state the lock was what
        /// guarded.
        template <class Write>
        auto under(Write write)
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            return write();
        }

        /// The frame's side: runs `write` under the lock, then wakes one worker.
        ///
        /// One and not all, because what a `give` adds is one piece of work. A caller that adds
        /// several calls this several times, which is what wakes several.
        template <class Write>
        void give(Write write)
        {
            under(std::move(write));
            mToWorker.notify_one();
        }

        /// A worker's side: runs `write` under the lock, then wakes the frame.
        template <class Write>
        void hand(Write write)
        {
            under(std::move(write));
            mToFrame.notify_all();
        }

        /// The frame's side: waits until `ready`.
        ///
        /// **False where the monitor closed**, which is a worker that threw or a run that is over.
        /// A frame that waited on a worker which had gone used to wait for ever, and it was safe
        /// only by an argument about which cells a caller could reject.
        template <class Ready>
        bool await(Ready ready)
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mToFrame.wait(lock, [&] { return mClosed || ready(); });

            return !mClosed;
        }

        /// A worker's whole loop: wait for work, pick it up, do it, and again until stopped.
        ///
        /// **`ready` and `take` share one lock hold**, because a second worker would otherwise
        /// empty the queue between them — and `turn` runs with the lock released, because a bake
        /// is tens of milliseconds and holding the lock across one would put the workers in single
        /// file.
        ///
        /// **A stop is answered before anything is picked up**, and not only where the queue is
        /// empty: the wait answers what `ready` said whether or not it was stopped, so a loop that
        /// went on from there would start one more turn that nobody is left to collect.
        ///
        /// @param ready whether there is anything to do. Under the lock.
        /// @param take what to pick up. Under the same lock hold, so nothing can take it first.
        /// @param turn what to do about it, handed the stop token so a long turn can give up part
        ///        way. Outside the lock, and it reaches this monitor again for what it hands back.
        template <class Ready, class Take, class Turn>
        void serve(std::stop_token stop, Ready ready, Take take, Turn turn)
        {
            for (;;)
            {
                try
                {
                    {
                        std::unique_lock<std::mutex> lock(mMutex);
                        if (!mToWorker.wait(lock, stop, [&] { return mClosed || ready(); }))
                            return;

                        if (mClosed || stop.stop_requested())
                            return;

                        take();
                    }

                    turn(stop);
                }
                catch (...)
                {
                    fail(std::current_exception());
                    return;
                }
            }
        }

        /// Wakes every waiter and refuses every wait after it. What a worker that threw does, and
        /// what an owner shutting down may do.
        void close()
        {
            under([&] { mClosed = true; });

            mToWorker.notify_all();
            mToFrame.notify_all();
        }

        /// Throws what the first worker to fail threw, and nothing where none did.
        ///
        /// **Thrown once.** The owner asks at a point where it can report, and a second ask after
        /// that has nothing to say.
        void rethrowFailure()
        {
            const std::exception_ptr failed = under([&] { return std::exchange(mFailed, nullptr); });
            if (failed != nullptr)
                std::rethrow_exception(failed);
        }

    private:
        /// Keeps the first failure and closes. **The first and not the last**, so the message names
        /// what actually went wrong rather than whichever worker finished after it.
        void fail(std::exception_ptr thrown)
        {
            under([&] {
                if (mFailed == nullptr)
                    mFailed = std::move(thrown);
            });

            close();
        }

        std::mutex mMutex;

        /// **`condition_variable_any` on the worker's side and the plain one on the frame's**,
        /// because only a worker waits on a stop token — and the any-form carries a second lock of
        /// its own that the frame has no use for.
        std::condition_variable_any mToWorker;
        std::condition_variable mToFrame;

        std::exception_ptr mFailed;
        bool mClosed = false;
    };
}
