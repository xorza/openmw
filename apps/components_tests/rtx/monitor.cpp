#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/monitor.hpp>
#include <components/rtx/worker.hpp>

namespace Rtx
{
    namespace
    {
        /// A queue over a monitor, which is the shape both of this renderer's workers take.
        ///
        /// **Held together rather than declared in each test**, so the worker is destroyed — and
        /// so joined — before the queue it reads, which is the rule `Worker` states.
        struct Served
        {
            Monitor mMonitor;
            std::deque<int> mPending;
            std::vector<int> mDone;

            /// What a turn does with what it took, so a test can make one throw or hold on the
            /// stop. Handed the token `serve` hands the turn.
            std::function<void(int, std::stop_token)> mTurn = [](int, std::stop_token) {};

            /// **Last, for the reason `Worker` gives.**
            Worker mWorker;

            void start()
            {
                mWorker.start([this](std::stop_token stop) {
                    int took = 0;

                    mMonitor.serve(
                        stop, [this] { return !mPending.empty(); },
                        [&] {
                            took = mPending.front();
                            mPending.pop_front();
                        },
                        [&](std::stop_token turn) {
                            mTurn(took, turn);
                            mMonitor.hand([&] { mDone.push_back(took); });
                        });
                });
            }

            void give(int one)
            {
                mMonitor.give([&] { mPending.push_back(one); });
            }

            /// Waits for `count` and answers them, or empty where the monitor closed first.
            std::vector<int> awaitDone(std::size_t count)
            {
                if (!mMonitor.await([&] { return mDone.size() >= count; }))
                    return {};

                return mMonitor.under([&] { return mDone; });
            }
        };

        /// Work given before the loop starts is not lost, and the order it is taken in is the order
        /// it arrived.
        ///
        /// **Given before the start on purpose.** A worker that only ever looked at the queue when
        /// it was woken would sit there for ever with three pieces of work in front of it, and a
        /// test that gave after starting would race past that.
        TEST(RtxMonitorTest, workGivenBeforeAWorkerStartsIsStillTaken)
        {
            Served served;
            served.give(1);
            served.give(2);
            served.give(3);

            served.start();

            EXPECT_EQ(served.awaitDone(3), (std::vector<int>{ 1, 2, 3 }));
        }

        /// A stop breaks the wait and the loop comes back, which is what every join rests on.
        TEST(RtxMonitorTest, aStopEndsALoopThatIsWaitingForWork)
        {
            Served served;
            served.start();

            served.give(7);
            EXPECT_EQ(served.awaitDone(1), (std::vector<int>{ 7 }));

            // The join is the assertion: it returns only where the wait was broken.
            served.mWorker.stop();
        }

        /// A turn that throws closes the monitor rather than ending the process, and the frame gets
        /// what it threw.
        ///
        /// **The whole of why a worker's exception is caught.** One out of a `std::jthread`'s body
        /// is a `std::terminate` that names nothing, and the frame would have waited for a result
        /// that was never coming.
        TEST(RtxMonitorTest, aTurnThatThrowsClosesTheMonitorAndTheFrameGetsIt)
        {
            Served served;
            served.mTurn = [](int one, std::stop_token) { throw std::runtime_error("turn " + std::to_string(one)); };
            served.start();

            served.give(4);

            // Empty, because the monitor closed before anything was ever done.
            EXPECT_TRUE(served.awaitDone(1).empty());

            EXPECT_THROW(served.mMonitor.rethrowFailure(), std::runtime_error);

            // Thrown once: the owner asks where it can report, and a second ask has nothing to say.
            EXPECT_NO_THROW(served.mMonitor.rethrowFailure());
        }

        /// A stop drops what is still queued rather than taking one more turn nobody collects.
        ///
        /// **The turn is what holds the join up.** A bake is tens of milliseconds, so a loop that
        /// went on from a wait the stop had already broken would make every shutdown wait one out.
        TEST(RtxMonitorTest, aStopLeavesQueuedWorkWhereItIs)
        {
            Served served;
            std::atomic<bool> running{ false };

            // Held until the stop is asked for, which is what puts the next two behind it.
            served.mTurn = [&](int, std::stop_token stop) {
                running = true;
                while (!stop.stop_requested())
                    std::this_thread::yield();
            };
            served.start();

            served.give(1);
            while (!running)
                std::this_thread::yield();

            served.give(2);
            served.give(3);

            served.mWorker.stop();

            // Read without the lock, because the join is what makes this the only thread left.
            EXPECT_EQ(served.mDone, (std::vector<int>{ 1 })) << "a stopped loop took work nobody was left to collect";
            EXPECT_EQ(served.mPending.size(), 2u);
        }

        /// A close wakes a frame that is waiting and answers false, rather than leaving it there.
        TEST(RtxMonitorTest, aCloseWakesAFrameThatIsWaitingForWhatWillNeverCome)
        {
            Monitor monitor;
            std::atomic<bool> waiting{ false };
            std::atomic<bool> answered{ true };

            std::jthread frame([&] {
                waiting = true;
                answered = monitor.await([] { return false; });
            });

            while (!waiting)
                std::this_thread::yield();

            monitor.close();
            frame.join();

            EXPECT_FALSE(answered) << "a wait against a closed monitor answers rather than holding";
        }
    }
}
