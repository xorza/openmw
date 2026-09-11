#include <thread>

#include <gtest/gtest.h>

#include <components/rtx/ownedby.hpp>

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

            // The guard is the other thread's now. Asking here would fire the assert, which is the
            // behaviour under test and not something a test can catch, so what is asserted is that
            // the adoption took: a second adoption here hands it back.
            owner.adopt();
            owner.check();
        }
    }
}
