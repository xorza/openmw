#include <components/misc/cellgrid.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace
{
    using namespace testing;
    using Misc::CellGrid;

    /// The rectangle the terrain is told, worked out by hand.
    ///
    /// **Minimum inclusive, maximum exclusive**, which is what the quad tree's own bounds test
    /// compares against — so a three-by-three square around (-3, -2) runs from -4 to -1 in x, and a
    /// grid a cell wide is not a rectangle of zero width.
    TEST(MiscCellGridTest, theBoundsRunFromTheCornerToOnePastTheOpposite)
    {
        EXPECT_EQ(CellGrid(osg::Vec2i(-3, -2), 1).getBounds(), osg::Vec4i(-4, -3, -1, 0));
        EXPECT_EQ(CellGrid(osg::Vec2i(0, 0), 0).getBounds(), osg::Vec4i(0, 0, 1, 1));
        EXPECT_EQ(CellGrid(osg::Vec2i(7, 22), 2).getBounds(), osg::Vec4i(5, 20, 10, 25));
    }

    /// The square holds exactly the cells within the half size, measured as a square and not a disc.
    TEST(MiscCellGridTest, aCellIsHeldWhenBothAxesAreInsideTheHalfSize)
    {
        const CellGrid grid(osg::Vec2i(-3, -2), 1);

        EXPECT_TRUE(grid.contains(-3, -2)) << "the centre";
        EXPECT_TRUE(grid.contains(-4, -1)) << "a corner, which a radius would have refused";
        EXPECT_TRUE(grid.contains(-2, -3)) << "the opposite corner";

        EXPECT_FALSE(grid.contains(-1, -2)) << "one column further out";
        EXPECT_FALSE(grid.contains(-3, 0)) << "one row further out";
    }

    /// Nearest first, ties broken by distance to the origin — which is the order the game loads a
    /// grid in, and so the order anything timing a camera across a boundary measures.
    ///
    /// Around the origin at half size one there are nine cells: the centre alone at distance zero,
    /// four at distance one, and four at distance two. Inside each band the tie is |x| + |y|, which
    /// is the same for all four of the first band and all four of the second, so what this pins is
    /// the bands rather than an order within one.
    TEST(MiscCellGridTest, theCellsComeOutNearestFirst)
    {
        std::vector<osg::Vec2i> cells;
        CellGrid(osg::Vec2i(0, 0), 1).listCells(cells);

        ASSERT_EQ(cells.size(), std::size_t{ 9 });
        EXPECT_EQ(cells.front(), osg::Vec2i(0, 0)) << "the centre is loaded first";

        const auto distance = [](const osg::Vec2i& cell) { return std::abs(cell.x()) + std::abs(cell.y()); };
        for (std::size_t at = 1; at < cells.size(); ++at)
            EXPECT_LE(distance(cells[at - 1]), distance(cells[at])) << "cell " << at << " comes before a nearer one";

        // The two bands, whichever order the four of each came out in.
        for (std::size_t at = 1; at < 5; ++at)
            EXPECT_EQ(distance(cells[at]), 1) << "cell " << at;
        for (std::size_t at = 5; at < 9; ++at)
            EXPECT_EQ(distance(cells[at]), 2) << "cell " << at;
    }

    /// The tie inside a band is distance to the origin, which is what separates two cells the centre
    /// is equally far from. Around (10, 0) the four at distance one are (9,0), (11,0), (10,-1) and
    /// (10,1); the first of them is the one nearest the origin, which is (9,0) at nine.
    TEST(MiscCellGridTest, aTieInsideABandGoesToTheCellNearestTheOrigin)
    {
        std::vector<osg::Vec2i> cells;
        CellGrid(osg::Vec2i(10, 0), 1).listCells(cells);

        ASSERT_EQ(cells.size(), std::size_t{ 9 });
        EXPECT_EQ(cells[1], osg::Vec2i(9, 0));
    }

    /// The buffer is refilled rather than appended to, because a caller on a frame path keeps it.
    TEST(MiscCellGridTest, listingTwiceLeavesOneSquareAndNotTwo)
    {
        std::vector<osg::Vec2i> cells;
        const CellGrid grid(osg::Vec2i(0, 0), 1);

        grid.listCells(cells);
        grid.listCells(cells);

        EXPECT_EQ(cells.size(), std::size_t{ 9 });
    }
}
