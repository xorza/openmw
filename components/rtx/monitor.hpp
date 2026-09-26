#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <utility>

namespace Rtx
{
    /// The lock between a frame and the workers it keeps, and the two waits across it — the dance
    /// and not the data, which stays with its owner. Every operation is a template on what it
    /// runs, so nothing here allocates on the frame's path. A worker that throws ends the process:
    /// nothing catches it, so `std::terminate` is called where it was thrown, before anything
    /// unwinds, and the crash catcher's report names the exception beside that thread's stack. A
    /// worker's failure is a bug or an allocation that failed; what the world supplies a worker
    /// refuses in its own results.
    class Monitor
    {
    public:
        Monitor() = default;

        /// Runs `write` under the lock and wakes nobody, answering with whatever it answered — by
        /// value, so a caller cannot hand back a reference into state the lock guarded.
        template <class Write>
        auto under(Write write)
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            return write();
        }

        /// The frame's side: runs `write` under the lock, then wakes one worker and not all,
        /// because what a `give` adds is one piece of work. A caller that adds several calls this
        /// several times, which is what wakes several.
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
        template <class Ready>
        void await(Ready ready)
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mToFrame.wait(lock, ready);
        }

        /// A worker's whole loop: wait for work, pick it up, do it, and again until stopped. `ready`
        /// and `take` share one lock hold, or a second worker would empty the queue between them;
        /// `turn` runs with the lock released, or a bake would put the workers in single file. A
        /// stop is answered before anything is picked up, or one more turn would start that nobody
        /// is left to collect.
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
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    if (!mToWorker.wait(lock, stop, ready) || stop.stop_requested())
                        return;

                    take();
                }

                turn(stop);
            }
        }

    private:
        std::mutex mMutex;

        /// `condition_variable_any` on the worker's side and the plain one on the frame's,
        /// because only a worker waits on a stop token — and the any-form carries a second lock of
        /// its own that the frame has no use for.
        std::condition_variable_any mToWorker;
        std::condition_variable mToFrame;
    };
}
