#include <cstdint>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/microtriangles.hpp>

namespace
{
    using namespace Rtx;

    /// Twice the area a microtriangle covers of the barycentric square, which is what its corners
    /// cross to.
    float doubleAreaOf(const Microtriangle& micro)
    {
        const osg::Vec2f first = micro.mCorners[1] - micro.mCorners[0];
        const osg::Vec2f second = micro.mCorners[2] - micro.mCorners[0];

        return std::abs(first.x() * second.y() - second.x() * first.y());
    }

    /// The curve numbers every microtriangle of a level exactly once.
    ///
    /// **This is the whole proof that the lattice and the curve agree.** The subdivision is walked
    /// row by row and the states are written at the curve's index, so a transcription error in the
    /// bit arithmetic — or a lattice that emitted a cell twice and another not at all — shows up as
    /// two microtriangles claiming one index and some index claiming none. Nothing else this side of
    /// the hardware would notice.
    TEST(RtxMicrotrianglesTest, everyLevelIsAsManyMicrotrianglesAsIndicesAndTheyPairOffExactly)
    {
        std::vector<Microtriangle> lattice;

        for (std::uint32_t level = 0; level <= 6; ++level)
        {
            subdivideTriangle(level, lattice);

            const std::size_t expected = std::size_t{ 1 } << (2 * level);
            ASSERT_EQ(lattice.size(), expected) << "level " << level;

            std::set<std::uint32_t> seen;
            for (const Microtriangle& micro : lattice)
                seen.insert(micro.mIndex);

            ASSERT_EQ(seen.size(), expected) << "level " << level << " indexed something twice";
            EXPECT_EQ(*seen.begin(), 0u) << "level " << level;
            EXPECT_EQ(*seen.rbegin(), expected - 1) << "level " << level;
        }
    }

    /// The cells tile the triangle: `4^level` of them, each a `4^level`-th of it.
    TEST(RtxMicrotrianglesTest, theCellsTileTheTriangleWithoutOverlapOrGap)
    {
        std::vector<Microtriangle> lattice;

        for (std::uint32_t level = 0; level <= 5; ++level)
        {
            subdivideTriangle(level, lattice);

            const auto share = 1.0f / static_cast<float>(1u << (2 * level));

            float total = 0.0f;
            for (const Microtriangle& micro : lattice)
            {
                // The triangle itself has twice-area one, so every cell of a level has `share`.
                EXPECT_NEAR(doubleAreaOf(micro), share, share * 1e-4f) << "level " << level;
                total += doubleAreaOf(micro);
            }

            EXPECT_NEAR(total, 1.0f, 1e-4f) << "level " << level;
        }
    }

    /// Every point inside a cell asks the curve for that cell, and not only its middle.
    ///
    /// The middle is what `subdivideTriangle` quantizes with, so a mapping that was right there and
    /// wrong a third of the way to a corner would pass every count above and still put a leaf's
    /// state on the microtriangle beside it. Nine points a cell, none of them on an edge.
    TEST(RtxMicrotrianglesTest, theCurveAnswersTheSameFromAnywhereInsideOneCell)
    {
        std::vector<Microtriangle> lattice;

        for (std::uint32_t level = 1; level <= 4; ++level)
        {
            subdivideTriangle(level, lattice);

            for (const Microtriangle& micro : lattice)
            {
                const osg::Vec2f middle = (micro.mCorners[0] + micro.mCorners[1] + micro.mCorners[2]) / 3.0f;

                for (const osg::Vec2f& corner : micro.mCorners)
                    for (const float towards : { 0.2f, 0.5f, 0.8f })
                    {
                        const osg::Vec2f at = middle * (1.0f - towards) + corner * towards;
                        EXPECT_EQ(microtriangleIndexAt(at.x(), at.y(), level), micro.mIndex)
                            << "level " << level << " at " << at.x() << ", " << at.y();
                    }
            }
        }
    }

    /// Level nought is the triangle itself, which is the compact form a resolved triangle takes.
    TEST(RtxMicrotrianglesTest, levelNoughtIsTheWholeTriangleAtIndexNought)
    {
        std::vector<Microtriangle> lattice;
        subdivideTriangle(0, lattice);

        ASSERT_EQ(lattice.size(), 1u);
        EXPECT_EQ(lattice[0].mIndex, 0u);
        EXPECT_EQ(lattice[0].mCorners[0], osg::Vec2f(0.0f, 0.0f));
        EXPECT_EQ(lattice[0].mCorners[1], osg::Vec2f(1.0f, 0.0f));
        EXPECT_EQ(lattice[0].mCorners[2], osg::Vec2f(0.0f, 1.0f));

        // Anywhere on the triangle, and past its edges, is the one microtriangle there is.
        EXPECT_EQ(microtriangleIndexAt(0.0f, 0.0f, 0), 0u);
        EXPECT_EQ(microtriangleIndexAt(0.9f, 0.05f, 0), 0u);
        EXPECT_EQ(microtriangleIndexAt(2.0f, -1.0f, 0), 0u);
    }

    /// The order the specification describes, written out: nearest the first corner, then the middle
    /// with its ordering flipped, then the second corner's, then the third's.
    ///
    /// **Four hand-derived indices, because a curve that is self-consistent can still be the wrong
    /// curve.** Every other test here would pass on any bijection; this one fails on all but the one
    /// the hardware reads by.
    TEST(RtxMicrotrianglesTest, theFirstLevelRunsCornerMiddleCornerCornerAsTheSpecificationSaysItDoes)
    {
        std::vector<Microtriangle> lattice;
        subdivideTriangle(1, lattice);

        ASSERT_EQ(lattice.size(), 4u);

        const auto indexOf
            = [&lattice](const osg::Vec2f& middle) { return microtriangleIndexAt(middle.x(), middle.y(), 1); };

        // Nearest the first corner, which barycentrics put at (0, 0).
        EXPECT_EQ(indexOf(osg::Vec2f(1.0f / 6.0f, 1.0f / 6.0f)), 0u);

        // The middle: the one cell of the four that points the other way.
        EXPECT_EQ(indexOf(osg::Vec2f(1.0f / 3.0f, 1.0f / 3.0f)), 1u);

        // Nearest the second corner, at (1, 0), and then the third, at (0, 1).
        EXPECT_EQ(indexOf(osg::Vec2f(2.0f / 3.0f, 1.0f / 6.0f)), 2u);
        EXPECT_EQ(indexOf(osg::Vec2f(1.0f / 6.0f, 2.0f / 3.0f)), 3u);
    }
}
