#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/shaders/scene.h>
#include <components/rtx/spriteshade.hpp>

namespace
{
    using namespace Rtx;

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
}
