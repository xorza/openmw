#include <optional>

#include <gtest/gtest.h>

#include <osg/BoundingSphere>
#include <osg/Vec3f>

#include "apps/openmw/mwrender/localmap.hpp"

namespace MWRender
{
    namespace
    {
        /// **A tile reaches the land as well as the scene.** The scene's sphere is what upstream
        /// drew between, and under a renderer whose ground is not in the graph it can stop above
        /// the ground. The land's own heights widen it and never narrow it.
        TEST(LocalMapDepthRangeTest, theLandWidensTheScenesRangeAndNeverNarrowsIt)
        {
            // A sphere centred at z = 100 with radius 50 spans 50 to 150.
            const osg::BoundingSphere scene(osg::Vec3f(0.f, 0.f, 100.f), 50.f);

            // Land from -200 to 120: the floor is below the sphere, the top inside it.
            const DepthRange below = mapDepthRange(scene, DepthRange{ -200.f, 120.f });
            EXPECT_FLOAT_EQ(below.mMin, -200.f);
            EXPECT_FLOAT_EQ(below.mMax, 150.f);

            // Land from 60 to 140, inside the sphere: the sphere's own range, unchanged.
            const DepthRange inside = mapDepthRange(scene, DepthRange{ 60.f, 140.f });
            EXPECT_FLOAT_EQ(inside.mMin, 50.f);
            EXPECT_FLOAT_EQ(inside.mMax, 150.f);

            // Land from 0 to 400: both ends past the sphere.
            const DepthRange around = mapDepthRange(scene, DepthRange{ 0.f, 400.f });
            EXPECT_FLOAT_EQ(around.mMin, 0.f);
            EXPECT_FLOAT_EQ(around.mMax, 400.f);
        }

        /// A cell with no land record draws between the sphere's heights, as upstream did, and a
        /// scene with nothing in it draws between the land's.
        TEST(LocalMapDepthRangeTest, whicheverOfTheTwoIsMissingTheOtherStands)
        {
            const osg::BoundingSphere scene(osg::Vec3f(0.f, 0.f, 100.f), 50.f);

            const DepthRange sceneOnly = mapDepthRange(scene, std::nullopt);
            EXPECT_FLOAT_EQ(sceneOnly.mMin, 50.f);
            EXPECT_FLOAT_EQ(sceneOnly.mMax, 150.f);

            const osg::BoundingSphere empty;
            ASSERT_FALSE(empty.valid());

            const DepthRange landOnly = mapDepthRange(empty, DepthRange{ -10.f, 30.f });
            EXPECT_FLOAT_EQ(landOnly.mMin, -10.f);
            EXPECT_FLOAT_EQ(landOnly.mMax, 30.f);
        }
    }
}
