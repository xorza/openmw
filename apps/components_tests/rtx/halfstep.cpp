#include <gtest/gtest.h>

#include "support/halfstep.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// The step the tolerance below is derived from, at the magnitudes a half float has: ten
        /// mantissa bits under the value's own power of two, and 2^-24 flat under 2^-14, where the
        /// exponent stops falling. One is 2^-10; 0.19 sits in [2^-3, 2^-2), so 2^-13; 0.001 in
        /// [2^-10, 2^-9), so 2^-20; and 65504, the largest half, in [2^15, 2^16), so 2^5.
        TEST(RtxHalfStepTest, theStepIsTheValuesOwnBinadeOverTenBits)
        {
            EXPECT_EQ(halfStepAt(1.0f), 0.0009765625f);
            EXPECT_EQ(halfStepAt(0.19f), 0.0001220703125f);
            EXPECT_EQ(halfStepAt(0.001f), 9.5367431640625e-07f);
            EXPECT_EQ(halfStepAt(65504.0f), 32.0f);

            // The smallest normal half, one under it, and nothing at all: the subnormal spacing.
            EXPECT_EQ(halfStepAt(6.103515625e-05f), 5.9604644775390625e-08f);
            EXPECT_EQ(halfStepAt(1.0e-6f), 5.9604644775390625e-08f);
            EXPECT_EQ(halfStepAt(0.0f), 5.9604644775390625e-08f);
        }
    }
}
