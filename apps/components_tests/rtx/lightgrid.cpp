#include <algorithm>

#include <gtest/gtest.h>

#include <components/rtx/lightgrid.hpp>
#include <components/rtx/scenedesc.hpp>

namespace Rtx
{
    namespace
    {
        /// What one cell's range holds, read the way the shader reads it — including the flat index,
        /// which is written out here rather than borrowed so that a change to it has to be made
        /// twice and noticed once.
        std::vector<std::uint32_t> lampsIn(const LightGrid& grid, std::uint32_t x, std::uint32_t y, std::uint32_t z)
        {
            const std::uint32_t flat = (z * grid.getSize().y() + y) * grid.getSize().x() + x;
            const std::span<const std::uint32_t> offsets = grid.getOffsets();

            return std::vector<std::uint32_t>(
                grid.getIndices().begin() + offsets[flat], grid.getIndices().begin() + offsets[flat + 1]);
        }

        Light lampAt(float x, float reach)
        {
            return Light{ .mPosition = osg::Vec3f(x, 0.0f, 0.0f), .mReach = reach };
        }

        /// A lamp is binned into every cell its reach touches, and into no others.
        ///
        /// **That is what makes the lookup complete.** A cell's list has to be every lamp that could
        /// light it, so the shader's own distance test refines the answer and never corrects it — a
        /// lamp binned only where it stands would go dark one cell away and leave a seam.
        ///
        /// Two lamps of reach 100 four thousand units apart put the grid's corner at -100 and its
        /// far edge at 4196, which is 4.2 cells of 1024 and so five of them. Each lamp spans 200
        /// units at one end, and the three cells between them hold nothing.
        TEST(RtxLightGridTest, aLampIsBinnedIntoEveryCellItsReachTouchesAndNoOthers)
        {
            const std::array lights{ lampAt(0.0f, 100.0f), lampAt(4096.0f, 100.0f) };
            const LightGrid grid(lights);

            EXPECT_EQ(grid.getOrigin(), osg::Vec3f(-100.0f, -100.0f, -100.0f));
            EXPECT_EQ(grid.getSize(), osg::Vec3ui(5u, 1u, 1u));
            EXPECT_FLOAT_EQ(grid.getInverseCell(), 1.0f / 1024.0f);

            EXPECT_EQ(lampsIn(grid, 0, 0, 0), std::vector<std::uint32_t>{ 0u });
            EXPECT_TRUE(lampsIn(grid, 1, 0, 0).empty()) << "the air between them";
            EXPECT_TRUE(lampsIn(grid, 2, 0, 0).empty());
            EXPECT_TRUE(lampsIn(grid, 3, 0, 0).empty());
            EXPECT_EQ(lampsIn(grid, 4, 0, 0), std::vector<std::uint32_t>{ 1u });

            // A prefix sum with a trailing sentinel: it starts at nothing, never goes backwards, and
            // ends at exactly what the runs hold, so the last cell needs no special case.
            const std::span<const std::uint32_t> offsets = grid.getOffsets();
            ASSERT_EQ(offsets.size(), 6u) << "one per cell, and one more";
            EXPECT_EQ(offsets[0], 0u);
            EXPECT_TRUE(std::is_sorted(offsets.begin(), offsets.end()));
            EXPECT_EQ(offsets.back(), grid.getIndices().size());
        }

        /// Every lamp that reaches a cell is in it, in the order they were given.
        TEST(RtxLightGridTest, aCellHoldsEveryLampThatReachesIt)
        {
            const std::array lights{ lampAt(0.0f, 300.0f), lampAt(200.0f, 300.0f) };
            const LightGrid grid(lights);

            ASSERT_EQ(grid.getSize(), osg::Vec3ui(1u, 1u, 1u)) << "one cell holds both reaches";
            EXPECT_EQ(lampsIn(grid, 0, 0, 0), (std::vector<std::uint32_t>{ 0u, 1u }));
        }

        /// An empty scene is a grid nothing can be found in, and asking is still legal.
        TEST(RtxLightGridTest, noLampsIsOneEmptyCell)
        {
            // Spelled out because `{}` would also name the unfilled grid, which is a different
            // thing: this is the one lamps were binned into and there were none.
            const LightGrid grid{ std::span<const Light>{} };

            EXPECT_EQ(grid.getSize(), osg::Vec3ui(1u, 1u, 1u));
            EXPECT_TRUE(grid.getIndices().empty());

            const std::span<const std::uint32_t> offsets = grid.getOffsets();
            ASSERT_EQ(offsets.size(), 2u) << "the one cell, and the sentinel";
            EXPECT_EQ(offsets[0], 0u);
            EXPECT_EQ(offsets[1], 0u);
        }

        /// The cell doubles until the grid fits, and there are two budgets to fit.
        ///
        /// **The second is not implied by the first.** Lamps spread across a world overrun the cell
        /// count while each of them is ordinary; a handful with enormous reaches overrun the entry
        /// count while the grid is still small, because each lands in every cell it touches.
        TEST(RtxLightGridTest, theCellDoublesUntilBothBudgetsFit)
        {
            // Seventy million units apart is 68,360 cells of 1024 along x and 34,180 of 2048, so the
            // cell count alone forces one doubling.
            const std::array wideLights{ lampAt(0.0f, 1.0f), lampAt(70.0e6f, 1.0f) };
            const LightGrid wide(wideLights);

            EXPECT_FLOAT_EQ(wide.getInverseCell(), 1.0f / 2048.0f) << "the cell count alone";
            EXPECT_EQ(wide.getSize().x(), 34180u);

            // And five lamps sharing one reach of 20,480 units, which is the case only the entry
            // budget catches. **The grid is a volume, so the cell budget is reached far sooner than
            // a plane would suggest** — 40 cells an axis is 64,000 of them, just inside the 65,536
            // allowed. Five lamps each covering all of that is 320,000 entries against 262,144, so
            // it doubles to 20 an axis: 8,000 cells and 40,000 entries.
            std::array<Light, 5> greedy{};
            for (Light& light : greedy)
                light.mReach = 20480.0f;

            const LightGrid crowded(greedy);
            EXPECT_FLOAT_EQ(crowded.getInverseCell(), 1.0f / 2048.0f) << "the entry count, at a legal cell count";
            EXPECT_EQ(crowded.getSize(), osg::Vec3ui(20u, 20u, 20u));
            EXPECT_EQ(crowded.getIndices().size(), 5u * 20u * 20u * 20u);
        }
    }
}
