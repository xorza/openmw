#include <thread>

#include <gtest/gtest.h>

#include <components/rtx/worker.hpp>

#include "death.hpp"

namespace Rtx
{
    namespace
    {
        /// A guard holds the thread that built it, and takes the one that adopts it.
        TEST(RtxOwnedByTest, aGuardHoldsTheThreadThatBuiltItUntilAnotherAdoptsIt)
        {
            OwnedBy owner;
            owner.check();

            std::jthread other([&] {
                owner.adopt();
                owner.check();
            });
            other.join();

#ifndef NDEBUG
            Testing::expectDies([&] { owner.check(); }, "a member touched from the wrong thread");
#endif

            owner.adopt();
            owner.check();
        }
    }
}
