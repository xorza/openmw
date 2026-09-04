#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/shaders/scene.h>
#include <components/rtx/spriteshade.hpp>

#include "allocations.hpp"

namespace Rtx
{
    namespace
    {
        /// One emitter and its run, built so the grid is one unit a cell.
        ///
        /// **A reach of sixteen over thirty-two cells is a cell of one unit**, as long as no sprite is
        /// wider than eight — which is what makes every figure below a plain number. The centre sits at
        /// the origin, so a sprite's world position is its offset on the grid.
        struct Column
        {
            std::vector<Shaders::GpuSprite> mSprites;
            Shaders::GpuEmitter mEmitter{};

            Column()
            {
                mEmitter.mCentre = osg::Vec3f();
                mEmitter.mReach = 16.0f;
            }

            void add(const osg::Vec3f& position, float radius, float alpha)
            {
                Shaders::GpuSprite sprite{};
                sprite.mPosition = position;
                sprite.mRadius = radius;
                sprite.mAlpha = alpha;
                mSprites.push_back(sprite);
                mEmitter.mCount = static_cast<std::uint32_t>(mSprites.size());
            }

            void shade(const osg::Vec3f& toSun)
            {
                SpriteShade shade;
                shadeWith(shade, toSun);
            }

            /// The same, through a shade the caller owns, so a test can run one twice.
            void shadeWith(SpriteShade& shade, const osg::Vec3f& toSun)
            {
                shade.shade(mSprites, std::span(&mEmitter, 1), toSun);
            }
        };

        const osg::Vec3f sEast(1.0f, 0.0f, 0.0f);

        /// A sprite behind another along the light reads that one's fade, and the one in front reads
        /// nothing.
        ///
        /// The near disc is eight in radius, and the far sprite's point sits a unit off its axis between
        /// two cells — both well inside the disc, where the footprint is whole, so the read is exactly
        /// the fade and not a rim's fraction of it. The near sprite is that unit lower, so from the sky
        /// it is the far one, and the other's disc of four does not reach it six across.
        TEST(RtxSpriteShadeTest, aSpriteBehindAnotherReadsItsFade)
        {
            Column column;
            column.add(osg::Vec3f(6.0f, 0.0f, -1.0f), 8.0f, 0.75f);
            column.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            column.shade(sEast);

            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f) << "nothing is nearer the sun";
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.75f) << "the near sprite's fade, once";
            EXPECT_FLOAT_EQ(column.mSprites[0].mSkyLayers, 0.0f) << "the higher one's disc does not reach it";
            EXPECT_FLOAT_EQ(column.mSprites[1].mSkyLayers, 0.0f) << "nothing is higher";
        }

        /// Layers add along the light, and the order the sprites arrive in is not the order they shade in.
        ///
        /// Three on the axis at twelve, six and nought, fading a half, a quarter and one: the last reads
        /// three quarters, the middle a half, the first nothing — from either end of the array.
        TEST(RtxSpriteShadeTest, layersAddUpAlongTheLightWhateverTheOrder)
        {
            const auto build = [](bool reversed) {
                Column column;
                const std::array<float, 3> along{ 12.0f, 6.0f, 0.0f };
                const std::array<float, 3> fade{ 0.5f, 0.25f, 1.0f };
                for (std::size_t i = 0; i < 3; ++i)
                {
                    const std::size_t at = reversed ? 2 - i : i;
                    column.add(osg::Vec3f(along[at], 0.0f, 0.0f), 4.0f, fade[at]);
                }
                column.shade(sEast);
                return column;
            };

            const Column forward = build(false);
            EXPECT_FLOAT_EQ(forward.mSprites[0].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(forward.mSprites[1].mSunLayers, 0.5f);
            EXPECT_FLOAT_EQ(forward.mSprites[2].mSunLayers, 0.75f);

            const Column backward = build(true);
            EXPECT_FLOAT_EQ(backward.mSprites[2].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(backward.mSprites[1].mSunLayers, 0.5f);
            EXPECT_FLOAT_EQ(backward.mSprites[0].mSunLayers, 0.75f);
        }

        /// A sprite beside the light's path to another is not in it.
        ///
        /// The near disc is two in radius and the far sprite four to the side of the axis: the rim's
        /// one-cell ramp reaches `2 + 0.5` and stops short of it.
        TEST(RtxSpriteShadeTest, aSpriteBesideTheLightsPathIsNotInIt)
        {
            Column column;
            column.add(osg::Vec3f(6.0f, 0.0f, 0.0f), 2.0f, 1.0f);
            column.add(osg::Vec3f(0.0f, 4.0f, 0.0f), 2.0f, 1.0f);
            column.shade(sEast);

            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.0f);
        }

        /// A disc too small to reach a cell's centre counts its own area on the cell it is in.
        ///
        /// A radius of a quarter cell is an area of `pi / 16 = 0.19635`. Both sprites sit on a cell's
        /// centre — half a unit off the axis, since the grid's cells are centred on whole numbers from
        /// the reach's edge — so the point lands where the far sprite reads, whole.
        TEST(RtxSpriteShadeTest, aTinyDiscCountsItsArea)
        {
            Column column;
            column.add(osg::Vec3f(6.0f, -0.5f, -0.5f), 0.25f, 1.0f);
            column.add(osg::Vec3f(0.0f, -0.5f, -0.5f), 8.0f, 1.0f);
            column.shade(sEast);

            EXPECT_NEAR(column.mSprites[1].mSunLayers, 0.19635f, 1.0e-4f);
            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f);
        }

        /// The sky is straight up, whatever the sun does.
        ///
        /// One sprite six units over another and one unit further from a low sun: the lower reads the
        /// upper's fade from the sky and nothing from the sun, and the upper reads nothing from either —
        /// the lower's disc is four in radius and the upper's path to the sun passes six above it.
        TEST(RtxSpriteShadeTest, theSkyIsStraightUp)
        {
            Column column;
            column.add(osg::Vec3f(-1.0f, 0.0f, 6.0f), 8.0f, 0.5f);
            column.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            column.shade(sEast);

            EXPECT_FLOAT_EQ(column.mSprites[1].mSkyLayers, 0.5f);
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(column.mSprites[0].mSkyLayers, 0.0f);
            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f);
        }

        /// A flame, a rain streak and a lone puff are shaded by nothing.
        TEST(RtxSpriteShadeTest, flamesStreaksAndLonePuffsAreNotShaded)
        {
            Column flame;
            flame.add(osg::Vec3f(6.0f, 0.0f, 0.0f), 8.0f, 1.0f);
            flame.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            flame.mEmitter.mAdditive = 1u;
            flame.shade(sEast);
            EXPECT_FLOAT_EQ(flame.mSprites[1].mSunLayers, 0.0f) << "a flame emits and shadows nothing";

            Column rain;
            rain.add(osg::Vec3f(6.0f, 0.0f, 0.0f), 8.0f, 1.0f);
            rain.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            rain.mEmitter.mAcross = osg::Vec3f(0.1f, 0.0f, 0.0f);
            rain.mEmitter.mUpward = osg::Vec3f(0.0f, 0.0f, -1.0f);
            rain.shade(sEast);
            EXPECT_FLOAT_EQ(rain.mSprites[1].mSunLayers, 0.0f) << "a streak is a thin thing";

            Column lone;
            lone.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            lone.shade(sEast);
            EXPECT_FLOAT_EQ(lone.mSprites[0].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(lone.mSprites[0].mSkyLayers, 0.0f);
        }

        /// Every cell of a disc holds the coverage the analysis gives it.
        ///
        /// **The cross-check the row's three parts need.** `layDown` walks a row as a ramp, a run the
        /// disc covers whole and a ramp again, and works the distance out only in the two ramps — so a
        /// wrong bound between them would put a whole cell at one where it should hold a fraction, or
        /// the other way about, and every test above reads a point deep inside a disc where both answers
        /// are the same. This reads the rim.
        ///
        /// **A probe of no alpha reads and lays nothing**, so one disc's footprint is what the whole
        /// grid holds however many points are put into it. The disc is further along the light than any
        /// of them, so it is laid before all of them.
        ///
        /// Half a unit off each axis is a cell's centre, where the read is that cell and not a blend of
        /// four — `aTinyDiscCountsItsArea` says why. A disc of six at a half of alpha is then
        /// `0.5 * clamp(6.5 - distance, 0, 1)` at a point `distance` cells away.
        TEST(RtxSpriteShadeTest, everyCellOfADiscHoldsTheCoverageTheAnalysisGivesIt)
        {
            struct Probe
            {
                float mAcross;
                float mUpward;
                float mExpected;
            };

            // Along one axis, so `distance` is the offset itself: whole out to five, the rim at six, and
            // nothing at seven.
            //
            // Then the diagonal, where three across and three up is `sqrt(18) = 4.2426` — still whole —
            // and four and four is `sqrt(32) = 5.6569`, which is `0.5 * 0.8431` on the rim.
            constexpr std::array<Probe, 6> sProbes{ {
                { 0.0f, 0.0f, 0.5f },
                { 5.0f, 0.0f, 0.5f },
                { 6.0f, 0.0f, 0.25f },
                { 7.0f, 0.0f, 0.0f },
                { 3.0f, 3.0f, 0.5f },
                { 4.0f, 4.0f, 0.5f * 0.84314575f },
            } };

            Column column;
            column.add(osg::Vec3f(6.0f, -0.5f, -0.5f), 6.0f, 0.5f);
            for (const Probe& probe : sProbes)
                column.add(osg::Vec3f(0.0f, -0.5f - probe.mAcross, -0.5f - probe.mUpward), 0.0f, 0.0f);

            column.shade(sEast);

            for (std::size_t at = 0; at < sProbes.size(); ++at)
                EXPECT_NEAR(column.mSprites[at + 1].mSunLayers, sProbes[at].mExpected, 1.0e-5f)
                    << "probe " << sProbes[at].mAcross << ", " << sProbes[at].mUpward;
        }

        /// A sun off every axis still finds what stands in its way.
        ///
        /// Two sprites on the diagonal, the nearer six root two along it: the same figures as on the
        /// axis, because the grid is laid across whatever the light is.
        TEST(RtxSpriteShadeTest, aSunOffTheAxesShadesAlongItself)
        {
            osg::Vec3f toSun(1.0f, 1.0f, 0.0f);
            toSun.normalize();

            Column column;
            column.add(toSun * 6.0f, 8.0f, 0.75f);
            column.add(osg::Vec3f(), 4.0f, 1.0f);
            column.shade(toSun);

            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.75f);
        }

        /// Two sprites at one depth shade in index order, and only one order is allowed.
        ///
        /// **What holds the sort's answer down, since the sort itself is not a stable one.** The order is made
        /// total by breaking a tie on the index, and a total order has one answer — so an unstable sort
        /// gives the stable one. Nothing above reaches a tie, and a sort that left equal depths in
        /// whichever order it found them would pass every one of them.
        ///
        /// Both sprites stand at one point, so both depths are equal along the sun and along the sky.
        /// The first lays half a layer where the second then reads it, and the second lays a whole one
        /// where nothing reads. Half a unit off each axis is a cell's centre, which is what makes the
        /// read the cell itself — `aTinyDiscCountsItsArea` says why.
        TEST(RtxSpriteShadeTest, twoSpritesAtOneDepthShadeInIndexOrder)
        {
            Column column;
            column.add(osg::Vec3f(0.5f, -0.5f, -0.5f), 6.0f, 0.5f);
            column.add(osg::Vec3f(0.5f, -0.5f, -0.5f), 6.0f, 1.0f);
            column.shade(sEast);

            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f) << "the lower index lays down first";
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.5f) << "and the higher one reads it";
            EXPECT_FLOAT_EQ(column.mSprites[0].mSkyLayers, 0.0f);
            EXPECT_FLOAT_EQ(column.mSprites[1].mSkyLayers, 0.5f);
        }

        /// Shading a run a second time goes to the heap not at all.
        ///
        /// **This runs on every frame, twice for every emitter that covers.** The scratch is kept and
        /// refilled, and the sort is the one thing that could break that: `std::stable_sort` takes a
        /// buffer off the heap however often it is called and however small the run is.
        ///
        /// Two sprites, because one is a run the shade passes over. Warmed up first, because the first
        /// of anything legitimately allocates: the scratch has to reach its size once.
        TEST(RtxSpriteShadeTest, shadingARunAgainDoesNotTouchTheHeap)
        {
            Column column;
            column.add(osg::Vec3f(6.0f, 0.0f, -1.0f), 8.0f, 0.75f);
            column.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);

            SpriteShade shade;
            column.shadeWith(shade, sEast);

            const std::size_t before = Testing::getAllocationCount();
            column.shadeWith(shade, sEast);
            const std::size_t after = Testing::getAllocationCount();

            EXPECT_EQ(after, before) << after - before << " allocations to shade a run";
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.75f) << "and the same answer as the first";
        }
    }
}
