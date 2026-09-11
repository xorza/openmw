#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/parallel.hpp>

namespace Rtx
{
    namespace
    {
        /// Every index runs exactly once, whichever hand took it.
        TEST(RtxParallelTest, everyIndexRunsOnceAndTheRunWaitsForAllOfThem)
        {
            constexpr std::size_t count = 64;
            std::vector<std::atomic<int>> ran(count);

            runInParallel(
                count, [] { return 0; }, [&](std::size_t at) { ++ran[at]; });

            for (std::size_t at = 0; at < count; ++at)
                EXPECT_EQ(ran[at].load(), 1) << "index " << at;
        }

        /// The first throw is what comes back, and every other index still runs.
        ///
        /// Index nought throws and the rest count themselves, so the count proves the run was not
        /// abandoned at the throw.
        TEST(RtxParallelTest, theFirstThrowComesBackAndTheRestOfTheBatchStillRuns)
        {
            constexpr std::size_t count = 32;
            std::atomic<std::size_t> ran{ 0 };

            const auto run = [&] {
                runInParallel(
                    count, [] { return 0; },
                    [&](std::size_t at) {
                        if (at == 0)
                            throw std::runtime_error("nought");

                        ++ran;
                    });
            };

            EXPECT_THROW(run(), std::runtime_error);
            EXPECT_EQ(ran.load(), count - 1);
        }

        /// Nothing to do is not a thread.
        TEST(RtxParallelTest, aRunOfNothingStartsNothing)
        {
            std::atomic<std::size_t> equipped{ 0 };

            runInParallel(
                0, [&] { return ++equipped; }, [](std::size_t) { FAIL() << "a body ran for no index"; });

            EXPECT_EQ(equipped.load(), 0u);
        }
    }
}
