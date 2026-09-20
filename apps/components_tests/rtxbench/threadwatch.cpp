#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxbench/threadwatch.hpp>

namespace Rtx
{
    namespace
    {
        /// Four takes a window, so the window closes on the fourth.
        constexpr double sStep = ThreadWindows::sWindowSeconds / 4.0;

        /// A run's clocks: the wall stepping by `sStep` a take, and three other threads' CPU —
        /// one the compile's, stepping by `share` of the wall, and two workers at a tenth each,
        /// which is what the sum would add up to something and the busiest must not.
        struct Clocks
        {
            double mSeconds = 0.0;
            std::vector<ThreadCpu> mThreads{ { 7, 0.0 }, { 12, 0.0 }, { 30, 0.0 } };

            void take(ThreadWindows& windows, const double share)
            {
                windows.take(mSeconds, mThreads);
                step(share);
            }

            void step(const double share)
            {
                mSeconds += sStep;
                mThreads[0].mSeconds += 0.1 * sStep;
                mThreads[1].mSeconds += share * sStep;
                mThreads[2].mSeconds += 0.1 * sStep;
            }

            void window(ThreadWindows& windows, const double share)
            {
                for (int at = 0; at < 4; ++at)
                    take(windows, share);
            }
        };

        TEST(RtxThreadWindowsTest, aWindowIsItsBusiestThreadAndNotTheSum)
        {
            ThreadWindows windows;
            Clocks clocks;

            // The anchor is the first take, so the first window closes on the take at 2.0 s: the
            // fifth, which is the first of the second call.
            clocks.window(windows, 0.1);
            EXPECT_TRUE(windows.getWindows().empty()) << "the first window has not closed";
            EXPECT_TRUE(windows.hasView());

            clocks.window(windows, 1.0);
            ASSERT_EQ(windows.getWindows().size(), 1u);
            EXPECT_DOUBLE_EQ(windows.getWindows()[0].mShare, 0.1) << "three at a tenth are 0.3 summed and 0.1 each";
            EXPECT_DOUBLE_EQ(windows.getWindows()[0].mSeconds, 2.0);

            clocks.take(windows, 0.0);
            ASSERT_EQ(windows.getWindows().size(), 2u);
            EXPECT_DOUBLE_EQ(windows.getWindows()[1].mShare, 1.0);
            EXPECT_DOUBLE_EQ(windows.getBusiestTotal(), 0.2 + 2.0);

            const ThreadShare share = windows.summarise();
            EXPECT_DOUBLE_EQ(share.mLargestShare, 1.0);
            EXPECT_DOUBLE_EQ(share.mLargestAt, 4.0);
            EXPECT_EQ(share.mWindows, 2u);
            EXPECT_EQ(describeThreads(share),
                "the busiest other thread took 1.00 of a core at most, at 4.0 s, over 2 windows, and held one flat "
                "out through 1 in a row");
        }

        TEST(RtxThreadWindowsTest, theSummaryCountsTheLongestStretchFlatOut)
        {
            ThreadWindows windows;
            Clocks clocks;

            // 0.1, then four flat out, a pause at half, then two flat out: the longest stretch is
            // four, and the pause is what keeps the two from joining it.
            clocks.window(windows, 0.1);
            for (int at = 0; at < 4; ++at)
                clocks.window(windows, 1.0);
            clocks.window(windows, 0.5);
            clocks.window(windows, 1.0);
            clocks.window(windows, 1.0);
            clocks.take(windows, 0.0);

            const ThreadShare share = windows.summarise();
            EXPECT_EQ(share.mWindows, 8u);
            EXPECT_EQ(share.mLongestFlatOut, 4u);
            EXPECT_DOUBLE_EQ(share.mLargestShare, 1.0);
            EXPECT_TRUE(share.mViewed);
            // The first of the equal largest, which is the window closed at 4.0 s.
            EXPECT_EQ(describeThreads(share),
                "the busiest other thread took 1.00 of a core at most, at 4.0 s, over 8 windows, and held one flat "
                "out through 4 in a row");

            ThreadWindows loading;
            Clocks route;
            route.window(loading, 0.81);
            route.window(loading, 0.81);
            route.take(loading, 0.0);
            EXPECT_EQ(loading.summarise().mLongestFlatOut, 0u) << "a loader at 0.81 is under the flat-out share";
        }

        TEST(RtxThreadWindowsTest, aThreadThatBeganInsideTheWindowIsNotMatched)
        {
            ThreadWindows windows;
            Clocks clocks;

            clocks.window(windows, 1.0);
            clocks.take(windows, 0.0);
            ASSERT_EQ(windows.getWindows().size(), 1u);

            // A thread with a core's worth already on its clock, begun after the anchor at 2.0 s:
            // read as nought and not as a hundred seconds taken inside this window.
            clocks.mThreads.push_back(ThreadCpu{ 99, 100.0 });
            for (int at = 0; at < 3; ++at)
                clocks.take(windows, 0.0);
            clocks.take(windows, 0.0);
            ASSERT_EQ(windows.getWindows().size(), 2u);
            EXPECT_DOUBLE_EQ(windows.getWindows()[1].mShare, 0.1) << "the workers, with the compile thread still";
        }

        TEST(RtxThreadWindowsTest, finishClosesAWindowLongEnoughToMeanSomething)
        {
            ThreadWindows windows;
            Clocks clocks;

            clocks.take(windows, 1.0);
            clocks.step(1.0);
            windows.finish(clocks.mSeconds, clocks.mThreads);
            ASSERT_EQ(windows.getWindows().size(), 1u) << "a second is over the shortest window";
            EXPECT_DOUBLE_EQ(windows.getWindows()[0].mShare, 1.0);
            EXPECT_DOUBLE_EQ(windows.getWindows()[0].mSeconds, 1.0);

            ThreadWindows sliver;
            Clocks again;
            again.take(sliver, 1.0);
            sliver.finish(0.25, again.mThreads);
            EXPECT_TRUE(sliver.getWindows().empty()) << "a quarter of a second answers nothing";
            EXPECT_EQ(describeThreads(sliver.summarise()), "no window of the process's other threads closed");
        }

        TEST(RtxThreadWindowsTest, aPlatformWithNoViewClosesNothingAndSaysSo)
        {
            ThreadWindows windows;
            for (int at = 0; at < 12; ++at)
                windows.take(at * sStep, {});
            windows.finish(6.0, {});

            EXPECT_FALSE(windows.hasView());
            EXPECT_TRUE(windows.getWindows().empty());
            EXPECT_EQ(describeThreads(windows.summarise()), "no view of the process's other threads on this platform");
        }

        TEST(RtxThreadWatchTest, thisProcessHasOtherThreadsWithClocks)
        {
            std::vector<ThreadCpu> threads;
            const std::uint64_t self = ThreadWatch::currentThread();
            ThreadWatch::readOtherThreads(threads, 0);
#ifdef __linux__
            for (std::size_t at = 1; at < threads.size(); ++at)
                EXPECT_LT(threads[at - 1].mThread, threads[at].mThread) << "not sorted";
            for (const ThreadCpu& thread : threads)
            {
                EXPECT_GE(thread.mSeconds, 0.0);
                EXPECT_NE(thread.mThread, self) << "the calling thread is not one of the others";
            }

            // A thread named to leave out is left out, whichever one it is.
            if (!threads.empty())
            {
                const std::uint64_t except = threads.front().mThread;
                std::vector<ThreadCpu> fewer;
                ThreadWatch::readOtherThreads(fewer, except);
                for (const ThreadCpu& thread : fewer)
                    EXPECT_NE(thread.mThread, except);
                EXPECT_EQ(fewer.size() + 1, threads.size());
            }
#else
            EXPECT_TRUE(threads.empty());
            EXPECT_EQ(self, 0u);
#endif
        }

        TEST(RtxThreadWatchTest, aWatchSamplesFromAThreadOfItsOwnAndAnswersOnceStopped)
        {
            // One other thread to see, because this process has none of its own: the watch leaves
            // out the frame's thread and its own sampler, and a view of nothing is no view.
            std::mutex idle;
            std::condition_variable_any wake;
            std::jthread other([&](std::stop_token stop) {
                std::unique_lock<std::mutex> lock(idle);
                wake.wait(lock, stop, [&stop] { return stop.stop_requested(); });
            });

            ThreadWatch watch;
            watch.start(ThreadWatch::currentThread());

            // The last reading is `stop`'s own, after the join, so a watch stopped at once has
            // seen the other thread whether or not the sampler's first tick came first.
            const ThreadWindows seen = watch.stop();
#ifdef __linux__
            EXPECT_TRUE(seen.hasView());
#else
            EXPECT_FALSE(seen.hasView());
#endif
            EXPECT_GE(seen.getSeconds(), 0.0);
            EXPECT_TRUE(seen.getWindows().empty()) << "nothing waited a window out";

            other.request_stop();
        }
    }
}
