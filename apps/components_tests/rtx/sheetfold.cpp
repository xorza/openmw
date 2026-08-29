#include <array>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/sheetfold.hpp>

namespace Rtx
{
    namespace
    {
        /// A unit quad, and the same four positions again as the vertices its back was modelled
        /// with — which is how the content spells a card: eight vertices, not four.
        const std::array<osg::Vec3f, 8> sCard{
            osg::Vec3f(0.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 1.0f, 0.0f),
        };

        const std::vector<std::uint32_t> sFront{ 0, 1, 2, 0, 2, 3 };

        TEST(RtxSheetFoldTest, aCardDoubledForItsBackKeepsTheFrontAndIsASheet)
        {
            SheetFold fold;

            // The back wound the other way, on the second set of vertices.
            std::vector<std::uint32_t> indices{ 0, 1, 2, 0, 2, 3, 6, 5, 4, 7, 6, 4 };
            EXPECT_TRUE(fold.fold(sCard, indices));
            EXPECT_EQ(indices, sFront) << "the copy the file wrote first is the one kept";

            // Folded again there is nothing left to pair, so a sheet is not a sheet twice.
            EXPECT_FALSE(fold.fold(sCard, indices));
            EXPECT_EQ(indices, sFront);
        }

        TEST(RtxSheetFoldTest, aTwinIsMatchedByItsCornersAndNotByWhereTheFileStartedIt)
        {
            SheetFold fold;

            // (0, 1, 2) reversed is (0, 2, 1), which the file may as well spell (2, 1, 0) or
            // (1, 0, 2): every rotation of it is the same back.
            for (const std::array<std::uint32_t, 3> back : { std::array<std::uint32_t, 3>{ 4, 6, 5 },
                     std::array<std::uint32_t, 3>{ 6, 5, 4 }, std::array<std::uint32_t, 3>{ 5, 4, 6 } })
            {
                std::vector<std::uint32_t> indices{ 0, 1, 2, back[0], back[1], back[2] };
                EXPECT_TRUE(fold.fold(sCard, indices));
                EXPECT_EQ(indices, (std::vector<std::uint32_t>{ 0, 1, 2 }));
            }

            // And the same triangle again with the same winding is a second front, not a back.
            std::vector<std::uint32_t> twice{ 0, 1, 2, 4, 5, 6 };
            EXPECT_FALSE(fold.fold(sCard, twice));
            EXPECT_EQ(twice, (std::vector<std::uint32_t>{ 0, 1, 2, 4, 5, 6 }));
        }

        TEST(RtxSheetFoldTest, aSolidHasNoTwinsAndAMixedShapeLosesOnlyItsTwins)
        {
            SheetFold fold;

            // A tetrahedron: four faces, no two over the same three corners.
            const std::array<osg::Vec3f, 4> tetra{
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            };
            std::vector<std::uint32_t> solid{ 0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3 };
            const std::vector<std::uint32_t> before = solid;
            EXPECT_FALSE(fold.fold(tetra, solid));
            EXPECT_EQ(solid, before);

            // A doubled card with one lone triangle beside it: the twin goes, the lone one stays,
            // and the shape is not a sheet — a leaf's stem is not lit through.
            std::vector<std::uint32_t> mixed{ 0, 1, 2, 2, 1, 0, 1, 2, 3 };
            EXPECT_FALSE(fold.fold(sCard, mixed));
            EXPECT_EQ(mixed, (std::vector<std::uint32_t>{ 0, 1, 2, 1, 2, 3 }));

            std::vector<std::uint32_t> none;
            EXPECT_FALSE(fold.fold(sCard, none));
            EXPECT_TRUE(none.empty());
        }
    }
}
