#include <cstdint>

#include <gtest/gtest.h>

#include <components/rtxvulkan/dispatch.hpp>

namespace Rtx
{
    namespace
    {
        /// **A workgroup covers its lanes exactly, and one lane past it costs a whole group.**
        /// Every compute pass in this backend launches by this, so a rounding that fell short would
        /// leave a stripe of the frame untouched.
        ///
        /// Hand-counted at a workgroup of eight: nothing needs none, one needs one, eight needs
        /// one, nine needs two, and 1920 needs 240.
        TEST(RtxDispatchTest, aDispatchCoversItsLanesAndNeverFallsShort)
        {
            EXPECT_EQ(groupsFor(0, 8), 0u);
            EXPECT_EQ(groupsFor(1, 8), 1u);
            EXPECT_EQ(groupsFor(7, 8), 1u);
            EXPECT_EQ(groupsFor(8, 8), 1u) << "a whole workgroup and no more";
            EXPECT_EQ(groupsFor(9, 8), 2u) << "one lane past it is a second group";
            EXPECT_EQ(groupsFor(1920, 8), 240u);

            // A workgroup of one is a group per lane, which is what the shape has to give back.
            EXPECT_EQ(groupsFor(37, 1), 37u);

            // Every lane covered and no group spare, at every extent and every workgroup — which is
            // the property, where the figures above are one reading of it.
            for (std::uint32_t workgroup = 1; workgroup <= 64; ++workgroup)
                for (std::uint32_t extent = 1; extent < 200; ++extent)
                {
                    const std::uint32_t groups = groupsFor(extent, workgroup);
                    ASSERT_GE(groups * workgroup, extent) << extent << " lanes of " << workgroup << " fell short";
                    ASSERT_LT((groups - 1) * workgroup, extent)
                        << extent << " lanes of " << workgroup << " took a group too many";
                }
        }
    }
}
