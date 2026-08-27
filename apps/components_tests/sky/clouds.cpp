#include <limits>

#include <gtest/gtest.h>

#include <components/sky/clouds.hpp>

namespace Sky
{
    namespace
    {
        /// The deck crosses on a share of the transition, and the share is the weather's own.
        ///
        /// **Morrowind's sky rolls in ahead of its light.** `Weather_<name>_Clouds_Maximum_Percent`
        /// is how much of a transition the deck's own crossing is spread over, so a storm's clouds
        /// are overhead while the ground is still lit for the weather it left. Both renderers cross
        /// their sheets on this, and only the game used to.
        TEST(RtxCloudsTest, theDeckCrossesOnItsOwnShareOfTheTransition)
        {
            // A weather that arrives over the first quarter is a deck four times ahead of it.
            EXPECT_FLOAT_EQ(cloudBlend(0.125f, 0.25f), 0.5f);
            EXPECT_FLOAT_EQ(cloudBlend(0.25f, 0.25f), 1.0f) << "arrived, with three quarters of the light to come";
            EXPECT_FLOAT_EQ(cloudBlend(0.0f, 0.25f), 0.0f) << "and it starts where the transition does";

            // A weather that spreads it over the whole transition crosses with it.
            EXPECT_FLOAT_EQ(cloudBlend(0.375f, 1.0f), 0.375f);

            // **Nothing recorded is not a rate.** The shipped fallbacks give ash and blight no
            // maximum percent, and a division by it is a NaN — which a rasterizer survives, because
            // a NaN opacity draws nothing and the old sky stays, and a tracer mixes its whole sky
            // by. Nothing recorded means the deck crosses at once.
            EXPECT_FLOAT_EQ(cloudBlend(0.5f, 0.0f), 1.0f);
            EXPECT_FLOAT_EQ(cloudBlend(0.5f, -1.0f), 1.0f) << "and neither is a negative share";
            EXPECT_FLOAT_EQ(cloudBlend(0.5f, std::numeric_limits<float>::quiet_NaN()), 1.0f);
        }
    }
}
