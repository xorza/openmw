#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <apps/openmw/mwrender/sceneframe.hpp>

namespace MWRender
{
    namespace
    {
        struct WaterCase
        {
            float mZ;
            float mHeight;
            bool mEnabled;
            bool mToggled;
            bool mUnderwater;
        };

        /// **One rule for what the water covers**: below the surface, and only where the cell has
        /// water and `twf` has not hidden it. At the surface itself the eye is not under it, which
        /// is `Water::isUnderwater`'s strict comparison.
        TEST(RtxWaterStateTest, theEyeIsUnderShownWaterAndStrictlyBelowIt)
        {
            constexpr WaterCase cases[] = {
                { -10.f, 0.f, true, true, true },
                { 10.f, 0.f, true, true, false },
                { 0.f, 0.f, true, true, false },
                { -10.f, 0.f, false, true, false },
                { -10.f, 0.f, true, false, false },
                { -10.f, 0.f, false, false, false },
                { 90.f, 100.f, true, true, true },
                { 110.f, 100.f, true, true, false },
            };

            for (const WaterCase& one : cases)
            {
                const WaterState water{ .mHeight = one.mHeight, .mEnabled = one.mEnabled, .mToggled = one.mToggled };
                EXPECT_EQ(water.isShown(), one.mEnabled && one.mToggled) << one.mZ << " under " << one.mHeight;
                EXPECT_EQ(water.isUnderwater(osg::Vec3f(0.f, 0.f, one.mZ)), one.mUnderwater)
                    << one.mZ << " under " << one.mHeight << " enabled " << one.mEnabled << " toggled " << one.mToggled;
            }
        }
    }
}
