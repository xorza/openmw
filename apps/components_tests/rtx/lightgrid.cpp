#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/lightgrid.hpp>
#include <components/rtx/scenedesc.hpp>

#include "allocations.hpp"

namespace Rtx
{
    namespace
    {
        /// What one cell's run holds, keyed the way the shader keys it — the flat index is written
        /// out here rather than borrowed so that a change to it has to be made twice and noticed
        /// once.
        std::vector<std::uint32_t> lampsIn(const LightGrid& grid, std::uint32_t x, std::uint32_t y, std::uint32_t z)
        {
            const std::uint32_t flat = (z * grid.getSize().y() + y) * grid.getSize().x() + x;
            const std::span<const std::uint32_t> run = grid.getList().getRun(flat);

            return std::vector<std::uint32_t>(run.begin(), run.end());
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
        /// far edge at 4196, which is 16.8 cells of 256 and so seventeen of them. Each lamp spans
        /// 200 units at one end, and the fifteen cells between them hold nothing.
        TEST(RtxLightGridTest, aLampIsBinnedIntoEveryCellItsReachTouchesAndNoOthers)
        {
            const std::array lights{ lampAt(0.0f, 100.0f), lampAt(4096.0f, 100.0f) };
            const LightGrid grid(lights);

            EXPECT_EQ(grid.getOrigin(), osg::Vec3f(-100.0f, -100.0f, -100.0f));
            EXPECT_EQ(grid.getSize(), osg::Vec3ui(17u, 1u, 1u));
            EXPECT_FLOAT_EQ(grid.getInverseCell(), 1.0f / 256.0f);

            EXPECT_EQ(lampsIn(grid, 0, 0, 0), std::vector<std::uint32_t>{ 0u });
            for (std::uint32_t x = 1; x < 16; ++x)
                EXPECT_TRUE(lampsIn(grid, x, 0, 0).empty()) << "the air between them, at cell " << x;
            EXPECT_EQ(lampsIn(grid, 16, 0, 0), std::vector<std::uint32_t>{ 1u });

            // A prefix sum with a trailing sentinel: the first run starts where the head ends, the
            // starts never go backwards, and the last one is where the list ends, so the last cell
            // needs no special case. Read as the device reads it, whole.
            const std::span<const std::uint32_t> list = grid.getList().getWhole();
            ASSERT_EQ(list.size(), 18u + 2u) << "one start per cell and one more, then the two entries";
            EXPECT_EQ(list[0], 18u);
            EXPECT_TRUE(std::is_sorted(list.begin(), list.begin() + 18));
            EXPECT_EQ(list[17], list.size());
        }

        /// Every lamp that reaches a cell is in it, in the order they were given.
        TEST(RtxLightGridTest, aCellHoldsEveryLampThatReachesIt)
        {
            // Reaches of 60 about 0 and 100 span -60 to 160, which is inside one cell of 256.
            const std::array lights{ lampAt(0.0f, 60.0f), lampAt(100.0f, 60.0f) };
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
            EXPECT_EQ(grid.getList().getEntryCount(), 0u);

            const std::span<const std::uint32_t> list = grid.getList().getWhole();
            ASSERT_EQ(list.size(), 2u) << "the one cell's start and the sentinel, and no run";
            EXPECT_EQ(list[0], 2u);
            EXPECT_EQ(list[1], 2u);
        }

        /// The cell doubles until the grid fits, and there are two budgets to fit.
        ///
        /// **The second is not implied by the first.** Lamps spread across a world overrun the cell
        /// count while each of them is ordinary; a handful with enormous reaches overrun the entry
        /// count while the grid is still small, because each lands in every cell it touches.
        TEST(RtxLightGridTest, theCellDoublesUntilBothBudgetsFit)
        {
            // Seventy million units apart is 273,438 cells of 256 along x, 68,360 of 1024 and 34,180
            // of 2048, so the cell count alone forces three doublings.
            const std::array wideLights{ lampAt(0.0f, 1.0f), lampAt(70.0e6f, 1.0f) };
            const LightGrid wide(wideLights);

            EXPECT_FLOAT_EQ(wide.getInverseCell(), 1.0f / 2048.0f) << "the cell count alone";
            EXPECT_EQ(wide.getSize().x(), 34180u);

            // And five lamps sharing one reach of 20,480 units, which is the case only the entry
            // budget catches. **The grid is a volume, so the cell budget is reached far sooner than
            // a plane would suggest** — the cell doubles to 1024 before 40 cells an axis is 64,000
            // of them, just inside the 65,536 allowed. Five lamps each covering all of that is
            // 320,000 entries against 262,144, so it doubles once more to 20 an axis: 8,000 cells
            // and 40,000 entries.
            std::array<Light, 5> greedy{};
            for (Light& light : greedy)
                light.mReach = 20480.0f;

            const LightGrid crowded(greedy);
            EXPECT_FLOAT_EQ(crowded.getInverseCell(), 1.0f / 2048.0f) << "the entry count, at a legal cell count";
            EXPECT_EQ(crowded.getSize(), osg::Vec3ui(20u, 20u, 20u));
            EXPECT_EQ(crowded.getList().getEntryCount(), 5u * 20u * 20u * 20u);
        }

        /// Three reaches at once: one lamp inside a corner of the grid, one against its far edge and
        /// one that covers the whole of it.
        ///
        /// **The three the binning has to get right at the same time.** `boxAround` runs once for
        /// the sizing, once for the count and once for the put, so a box the three do not agree on
        /// is a lamp in the wrong cells — and the case that would show it is a lamp whose box is
        /// clamped at one end and not the other.
        ///
        /// The grid, by hand. A reaches x[-512, 512] and B x[3584, 4608], both y and z [-512, 512];
        /// C reaches x[-512, 4608] and y and z [-2560, 2560]. So the corner is (-512, -2560, -2560)
        /// and the far edge is (4608, 2560, 2560): an extent of 5120 on every axis, which is 20 cells
        /// of 256 each way. Eight thousand cells and 8,225 entries, both inside the first cell
        /// size's budgets, so nothing doubles.
        TEST(RtxLightGridTest, lampsSpanningOneCellSeveralAndTheWholeGridAreEachBinnedRight)
        {
            const std::array lights{ lampAt(0.0f, 512.0f), lampAt(4096.0f, 512.0f), lampAt(2048.0f, 2560.0f) };
            const LightGrid grid(lights);

            ASSERT_EQ(grid.getOrigin(), osg::Vec3f(-512.0f, -2560.0f, -2560.0f));
            ASSERT_EQ(grid.getSize(), osg::Vec3ui(20u, 20u, 20u));
            EXPECT_FLOAT_EQ(grid.getInverseCell(), 1.0f / 256.0f);

            // A spans x cells [0, 5) and y and z cells [8, 13), which is 125 cells. B spans x cells
            // [16, 20) — its box reaches 21 and the grid is twenty across, so the clamp is what keeps
            // it inside — and the same five in y and z, which is 100. C covers all eight thousand.
            EXPECT_EQ(grid.getList().getEntryCount(), 125u + 100u + 8000u);

            EXPECT_EQ(lampsIn(grid, 0, 8, 8), (std::vector<std::uint32_t>{ 0u, 2u }))
                << "the near lamp and the wide one";
            EXPECT_EQ(lampsIn(grid, 4, 12, 12), (std::vector<std::uint32_t>{ 0u, 2u })) << "the far corner of A's box";
            EXPECT_EQ(lampsIn(grid, 19, 10, 10), (std::vector<std::uint32_t>{ 1u, 2u })) << "the clamped edge cell";
            EXPECT_EQ(lampsIn(grid, 10, 10, 10), std::vector<std::uint32_t>{ 2u })
                << "the air between the two small ones";
            EXPECT_EQ(lampsIn(grid, 0, 0, 0), std::vector<std::uint32_t>{ 2u }) << "below A, which reaches only to y 8";
            EXPECT_EQ(lampsIn(grid, 19, 19, 19), std::vector<std::uint32_t>{ 2u });
        }

        /// A rebind of the same lamps goes nowhere near the allocator.
        ///
        /// **What a frame that moves does**, and the claim `rebuild` is written against: assigning a
        /// freshly built grid over this one threw the run list's vectors away and made them again,
        /// on every frame that moved.
        TEST(RtxLightGridTest, rebindingTheSameLampsDoesNotTouchTheHeap)
        {
            const std::array lights{ lampAt(0.0f, 512.0f), lampAt(4096.0f, 512.0f), lampAt(2048.0f, 2560.0f) };

            LightGrid grid;
            grid.rebuild(lights);
            grid.rebuild(lights);

            const std::size_t before = Testing::getAllocationCount();
            grid.rebuild(lights);
            const std::size_t spent = Testing::getAllocationCount() - before;

            EXPECT_EQ(spent, 0u) << spent << " allocations to bin the lamps a frame already held";
            EXPECT_EQ(grid.getList().getEntryCount(), 125u + 100u + 8000u) << "and it binned them all the same";
        }

        /// A lamp that only flickered is not binned again, and one that moved is.
        ///
        /// **This is what the whole gate rests on**: the grid reads a light's position and its reach
        /// and nothing else, so a colour that changed leaves the same grid — and Morrowind's lamps
        /// change colour on nearly every frame, which is what made a per-frame rebind the ordinary
        /// case rather than the exception.
        ///
        /// The two are told apart by what the grid comes to: a lamp moved a whole cell lands in
        /// different cells, and the entry count says so.
        TEST(RtxLightGridTest, aLampThatOnlyFlickeredIsNotBinnedAgain)
        {
            std::array lights{ lampAt(0.0f, 512.0f), lampAt(4096.0f, 512.0f) };

            LightGrid grid;
            grid.rebuild(lights);

            const std::vector<std::uint32_t> first = lampsIn(grid, 0, 0, 0);

            // What a flicker writes, and nothing the grid ever reads.
            lights[0].mIntensity = osg::Vec3f(9.0f, 3.0f, 1.0f);
            lights[1].mSourceRadius = 7.0f;
            lights[1].mClearance = 2.0f;
            grid.rebuild(lights);

            EXPECT_EQ(lampsIn(grid, 0, 0, 0), first) << "a lamp that only changed colour moved the grid";

            // And a lamp that reaches further is a lamp the grid has to be made for again: 512 over
            // a cell of 256 spans five cells across, and 2048 spans seventeen.
            const std::size_t reachedTwo = grid.getList().getEntryCount();
            lights[0].mReach = 2048.0f;
            grid.rebuild(lights);

            EXPECT_NE(grid.getList().getEntryCount(), reachedTwo) << "a lamp that reaches further was not rebinned";
        }
    }
}
