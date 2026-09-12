#include "fallbackseed.hpp"

#include <gtest/gtest.h>

#include <components/fallback/fallback.hpp>

namespace TestingOpenMW
{
    namespace
    {
        /// Plants the seed before any test runs, from inside a file of this fork's rather than from
        /// the test binary's `main`.
        ///
        /// **A global environment and not a fixture**, because `Fallback::Map::init` keeps whichever
        /// value arrives first: the seed has to land before the first test that reads the map, and
        /// gtest runs every registered environment's `SetUp` before the first test. Registered from
        /// a static initialiser, which is what makes both binaries pick it up by listing this file.
        class SeedFallback : public ::testing::Environment
        {
        public:
            void SetUp() override { Fallback::Map::init(fallbackSeed()); }
        };

        const ::testing::Environment* const sSeeded = ::testing::AddGlobalTestEnvironment(new SeedFallback);
    }
}
