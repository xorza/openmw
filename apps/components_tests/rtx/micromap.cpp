#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/alphabounds.hpp>
#include <components/rtx/alphaimage.hpp>
#include <components/rtx/micromap.hpp>
#include <components/rtx/texturedata.hpp>

namespace
{
    using namespace Rtx;

    /// What the device would allow, which is well past what `sSubdivisionCeiling` takes.
    constexpr std::uint32_t sDeviceLimit = 12;

    constexpr float sHalf = 0.5f;

    /// One uncompressed level, with alpha stated per texel.
    struct OneLevelMask
    {
        std::vector<std::byte> mBytes;
        MipLevel mLevel;

        OneLevelMask(std::uint32_t extent, const std::function<std::uint8_t(std::uint32_t, std::uint32_t)>& alpha)
            : mLevel{ 0, extent, extent }
        {
            for (std::uint32_t y = 0; y < extent; ++y)
                for (std::uint32_t x = 0; x < extent; ++x)
                {
                    mBytes.insert(mBytes.end(), 3, std::byte{ 0 });
                    mBytes.push_back(std::byte{ alpha(x, y) });
                }
        }

        TextureData describe() const
        {
            return TextureData{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = mLevel.mWidth,
                .mHeight = mLevel.mHeight,
                .mBytes = mBytes,
                .mLevels = std::span(&mLevel, 1),
            };
        }
    };

    /// The triangle a whole texture is stretched over, which is the half of the square below its
    /// diagonal — and so the one whose own box takes in a corner it never reaches.
    const std::array<osg::Vec2f, 3> sUnitTriangle{
        osg::Vec2f(0.0f, 0.0f),
        osg::Vec2f(1.0f, 0.0f),
        osg::Vec2f(0.0f, 1.0f),
    };

    const std::array<std::uint32_t, 3> sOneTriangle{ 0, 1, 2 };

    const osg::Vec4f sNoTransform{ 1.0f, 1.0f, 0.0f, 0.0f };

    /// One wide band of material, with everything outside it a hole.
    ///
    /// **Wide, and with its edges deliberately off any subdivision lattice.** What a microtriangle
    /// asks about is the box around it plus the ring a bilinear read reaches, so a band narrower
    /// than that box has no interior for anything to resolve — every cell would take in an edge,
    /// come back unknown, and the collapse would fold the whole triangle away again. A band whose
    /// edges sit on the lattice does the same to the cells that meet them.
    auto band(std::uint32_t from, std::uint32_t to)
    {
        return [from, to](std::uint32_t x, std::uint32_t) -> std::uint8_t { return x >= from && x < to ? 255 : 0; };
    }

    /// A triangle whose mask says the same thing everywhere carries one state and is not cut at all.
    TEST(RtxMicromapTest, aTriangleWhollyOnTheMaterialIsOneStateAndNoSubdivision)
    {
        const OneLevelMask solid(32, [](std::uint32_t, std::uint32_t) { return 255; });
        const AlphaBounds bounds(AlphaImage(solid.describe()), sHalf);

        const Micromap micromap(sUnitTriangle, sOneTriangle, sNoTransform, bounds, sDeviceLimit);

        ASSERT_EQ(micromap.getTriangles().size(), 1u);
        EXPECT_EQ(micromap.getTriangles()[0].mSubdivisionLevel, 0u);
        EXPECT_EQ(micromap.at(0, 0), Opacity::Opaque);

        // One state is two bits, and two bits still have to sit in a byte.
        ASSERT_EQ(micromap.getData().size(), 1u);
        EXPECT_EQ(micromap.getData()[0], std::byte{ 0x01 });

        ASSERT_EQ(micromap.getUsage().size(), 1u);
        EXPECT_EQ(micromap.getUsage()[0].mCount, 1u);
        EXPECT_EQ(micromap.getUsage()[0].mSubdivisionLevel, 0u);

        EXPECT_DOUBLE_EQ(micromap.getTally().mOpaque, 1.0);
        EXPECT_DOUBLE_EQ(micromap.getTally().mUnknown, 0.0);
    }

    /// And the other way round, which is the case that pays: the empty middle of the card a leaf is
    /// painted on is where a canopy's traversal actually goes.
    TEST(RtxMicromapTest, aTriangleWhollyInAHoleIsOneStateToo)
    {
        const OneLevelMask empty(32, [](std::uint32_t, std::uint32_t) { return 0; });
        const AlphaBounds bounds(AlphaImage(empty.describe()), sHalf);

        const Micromap micromap(sUnitTriangle, sOneTriangle, sNoTransform, bounds, sDeviceLimit);

        ASSERT_EQ(micromap.getTriangles().size(), 1u);
        EXPECT_EQ(micromap.getTriangles()[0].mSubdivisionLevel, 0u);
        EXPECT_EQ(micromap.at(0, 0), Opacity::Transparent);
        EXPECT_EQ(micromap.getData()[0], std::byte{ 0x00 });
        EXPECT_DOUBLE_EQ(micromap.getTally().mTransparent, 1.0);
    }

    /// A mask the triangle straddles is cut up, and only what straddles goes on asking.
    ///
    /// The three states have to all be there: the inside of a stripe is material, the inside of the
    /// gap beside it is hole, and the column where they meet is neither — which is the boundary the
    /// cutout shader keeps, and the whole of what a micromap leaves it.
    TEST(RtxMicromapTest, aTriangleThatStraddlesIsCutUpAndOnlyTheBoundaryGoesOnAsking)
    {
        const OneLevelMask banded(64, band(5, 37));
        const AlphaBounds bounds(AlphaImage(banded.describe()), sHalf);

        const Micromap micromap(sUnitTriangle, sOneTriangle, sNoTransform, bounds, sDeviceLimit);

        ASSERT_EQ(micromap.getTriangles().size(), 1u);

        // Half of a 64-square is 2048 texels, and sixteen texels a microtriangle wants 128 of them:
        // four levels, which is 256.
        ASSERT_EQ(micromap.getTriangles()[0].mSubdivisionLevel, 4u);
        ASSERT_EQ(micromap.getData().size(), 256u / 4u);

        std::array<std::uint32_t, 3> counts{};
        for (std::uint32_t index = 0; index < 256; ++index)
            ++counts[static_cast<std::size_t>(micromap.at(0, index))];

        EXPECT_GT(counts[static_cast<std::size_t>(Opacity::Opaque)], 0u);
        EXPECT_GT(counts[static_cast<std::size_t>(Opacity::Transparent)], 0u);
        EXPECT_GT(counts[static_cast<std::size_t>(Opacity::Unknown)], 0u);
        EXPECT_EQ(counts[0] + counts[1] + counts[2], 256u);

        // Two bits each from the least significant end of a byte, which is how the hardware reads
        // them — a packing that ran the other way would put every state in its neighbour's place.
        const auto first = static_cast<std::uint8_t>(micromap.getData()[0]);
        for (std::uint32_t index = 0; index < 4; ++index)
            EXPECT_EQ(static_cast<Opacity>((first >> (2 * index)) & 0x3u), micromap.at(0, index)) << "state " << index;

        EXPECT_DOUBLE_EQ(
            micromap.getTally().mOpaque + micromap.getTally().mTransparent + micromap.getTally().mUnknown, 1.0);
    }

    /// **A triangle whose pieces all agree collapses, and its own box did not catch it.** A triangle
    /// covers half the square its coordinates span, so the box around it takes in a corner the
    /// triangle never reaches — and a hole in that corner makes the one query above the subdivision
    /// say "straddles" about geometry that is solid the whole way across.
    TEST(RtxMicromapTest, everyPieceAgreeingCollapsesBackToOneState)
    {
        // Empty only in the corner the diagonal cuts off, and material everywhere the triangle is.
        // Clear of the far edge as well, because the ring wraps: a hole against it reaches back
        // round to the corner the triangle starts at, and every cell there would go on asking.
        const OneLevelMask corner(
            32, [](std::uint32_t x, std::uint32_t y) { return x >= 20 && x < 30 && y >= 20 && y < 30 ? 0 : 255; });
        const AlphaBounds bounds(AlphaImage(corner.describe()), sHalf);

        // The box the triangle spans reaches that corner, so the cheap answer is that it straddles.
        ASSERT_EQ(bounds.classify(UvBox{ 0.0f, 0.0f, 1.0f, 1.0f }), Opacity::Unknown);

        const Micromap micromap(sUnitTriangle, sOneTriangle, sNoTransform, bounds, sDeviceLimit);

        ASSERT_EQ(micromap.getTriangles().size(), 1u);
        EXPECT_EQ(micromap.getTriangles()[0].mSubdivisionLevel, 0u);
        EXPECT_EQ(micromap.at(0, 0), Opacity::Opaque);
        EXPECT_EQ(micromap.getData().size(), 1u);
    }

    /// How finely a triangle is cut follows how much of the mask it covers, and nothing else.
    ///
    /// Two triangles over one mask, sixteen times apart in area: the level has to differ, or the
    /// target is not doing anything and every triangle is being cut to the same depth.
    TEST(RtxMicromapTest, theCutFollowsHowManyTexelsATriangleCovers)
    {
        const OneLevelMask banded(64, band(5, 37));
        const AlphaBounds bounds(AlphaImage(banded.describe()), sHalf);

        const std::array<osg::Vec2f, 6> texCoords{
            osg::Vec2f(0.0f, 0.0f),
            osg::Vec2f(1.0f, 0.0f),
            osg::Vec2f(0.0f, 1.0f),
            osg::Vec2f(0.0f, 0.0f),
            osg::Vec2f(0.25f, 0.0f),
            osg::Vec2f(0.0f, 0.25f),
        };
        const std::array<std::uint32_t, 6> indices{ 0, 1, 2, 3, 4, 5 };

        const Micromap micromap(texCoords, indices, sNoTransform, bounds, sDeviceLimit);

        ASSERT_EQ(micromap.getTriangles().size(), 2u);

        // 2048 texels wants four levels; a sixteenth of the area is 128 texels and wants two.
        EXPECT_EQ(micromap.getTriangles()[0].mSubdivisionLevel, 4u);
        EXPECT_EQ(micromap.getTriangles()[1].mSubdivisionLevel, 2u);
        EXPECT_NE(micromap.getTriangles()[0].mSubdivisionLevel, micromap.getTriangles()[1].mSubdivisionLevel);

        // Two levels used, so the build has to be told about both of them.
        ASSERT_EQ(micromap.getUsage().size(), 2u);
        EXPECT_EQ(micromap.getUsage()[0].mSubdivisionLevel, 2u);
        EXPECT_EQ(micromap.getUsage()[0].mCount, 1u);
        EXPECT_EQ(micromap.getUsage()[1].mSubdivisionLevel, 4u);
        EXPECT_EQ(micromap.getUsage()[1].mCount, 1u);

        // The second triangle's states start where the first triangle's end.
        EXPECT_EQ(micromap.getTriangles()[0].mDataOffset, 0u);
        EXPECT_EQ(micromap.getTriangles()[1].mDataOffset, 256u / 4u);
    }

    /// The material's texture transform is applied before the mask is read, because it is what says
    /// which part of the mask the triangle is wearing.
    TEST(RtxMicromapTest, theTextureTransformDecidesWhichPartOfTheMaskIsRead)
    {
        // Opaque in its first eight columns and empty in the rest.
        const OneLevelMask edged(32, [](std::uint32_t x, std::uint32_t) { return x < 8 ? 255 : 0; });
        const AlphaBounds bounds(AlphaImage(edged.describe()), sHalf);

        const Micromap stretched(sUnitTriangle, sOneTriangle, sNoTransform, bounds, sDeviceLimit);
        ASSERT_EQ(stretched.getTriangles().size(), 1u);
        EXPECT_GT(stretched.getTriangles()[0].mSubdivisionLevel, 0u) << "the whole mask straddles";

        // Folded down into texels two to six, which are clear of both the seam at eight and the one
        // the wrap makes at nought.
        const Micromap folded(
            sUnitTriangle, sOneTriangle, osg::Vec4f(0.125f, 0.125f, 0.0625f, 0.0625f), bounds, sDeviceLimit);

        ASSERT_EQ(folded.getTriangles().size(), 1u);
        EXPECT_EQ(folded.getTriangles()[0].mSubdivisionLevel, 0u);
        EXPECT_EQ(folded.at(0, 0), Opacity::Opaque);
    }

    /// Geometry whose cutout cannot be answered for goes on asking, and no micromap is built for it.
    ///
    /// **Empty means build none, not build all transparent.** A mesh with no texture coordinates has
    /// nowhere to read the mask, and a mask that could not be decoded has nothing in it — deciding
    /// either way for them would put a hole through a surface nobody could check.
    TEST(RtxMicromapTest, thereIsNoMicromapForGeometryNothingCanAnswerFor)
    {
        const OneLevelMask solid(32, [](std::uint32_t, std::uint32_t) { return 255; });
        const AlphaBounds bounds(AlphaImage(solid.describe()), sHalf);

        EXPECT_TRUE(Micromap({}, sOneTriangle, sNoTransform, bounds, sDeviceLimit).isEmpty());
        EXPECT_TRUE(Micromap(sUnitTriangle, {}, sNoTransform, bounds, sDeviceLimit).isEmpty());

        const AlphaBounds nothing(AlphaImage(TextureData{}), sHalf);
        EXPECT_TRUE(Micromap(sUnitTriangle, sOneTriangle, sNoTransform, nothing, sDeviceLimit).isEmpty());
    }

    /// A device that will not build past a level is what decides, under the ceiling this keeps.
    TEST(RtxMicromapTest, theDeviceLimitAndTheCeilingBothHoldTheCutBack)
    {
        const OneLevelMask banded(64, band(5, 37));
        const AlphaBounds bounds(AlphaImage(banded.describe()), sHalf);

        const Micromap held(sUnitTriangle, sOneTriangle, sNoTransform, bounds, 2);
        ASSERT_EQ(held.getTriangles().size(), 1u);
        EXPECT_EQ(held.getTriangles()[0].mSubdivisionLevel, 2u) << "the device would not build four";

        // A mask fine enough to want more levels than the ceiling allows still stops at it.
        const OneLevelMask wide(1024, band(80, 592));
        const AlphaBounds wideBounds(AlphaImage(wide.describe()), sHalf);

        const Micromap capped(sUnitTriangle, sOneTriangle, sNoTransform, wideBounds, sDeviceLimit);
        ASSERT_EQ(capped.getTriangles().size(), 1u);
        EXPECT_EQ(capped.getTriangles()[0].mSubdivisionLevel, Micromap::sSubdivisionCeiling);
    }
}
