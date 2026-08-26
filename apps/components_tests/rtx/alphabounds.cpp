#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/alphabounds.hpp>
#include <components/rtx/alphaimage.hpp>
#include <components/rtx/texturedata.hpp>

namespace
{
    using namespace Rtx;

    /// A mask stated level by level, the way a `.dds` with its chain already built arrives.
    ///
    /// Uncompressed, so that a test asserts the alpha it wrote rather than what a block could
    /// express, and stated rather than averaged — a level that disagrees with the one above it is
    /// exactly what the bound has to notice.
    struct StatedMask
    {
        std::vector<std::byte> mBytes;
        std::vector<MipLevel> mLevels;

        void add(std::uint32_t width, std::uint32_t height,
            const std::function<std::uint8_t(std::uint32_t, std::uint32_t)>& alpha)
        {
            mLevels.push_back(MipLevel{ static_cast<std::uint32_t>(mBytes.size()), width, height });

            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    mBytes.insert(mBytes.end(), 3, std::byte{ 0 });
                    mBytes.push_back(std::byte{ alpha(x, y) });
                }
        }

        TextureData describe() const
        {
            return TextureData{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = mLevels.empty() ? 0 : mLevels.front().mWidth,
                .mHeight = mLevels.empty() ? 0 : mLevels.front().mHeight,
                .mBytes = mBytes,
                .mLevels = mLevels,
            };
        }
    };

    /// A patch one texel across, on the texel a coordinate lands in.
    UvBox oneTexelAt(float u, float v)
    {
        return UvBox{ u, v, u, v };
    }

    constexpr float sHalf = 0.5f;

    /// The two plain answers, on a mask with nothing to straddle.
    TEST(RtxAlphaBoundsTest, aPatchWhollyOnTheMaterialIsOpaqueAndOneWhollyInAHoleIsTransparent)
    {
        StatedMask solid;
        solid.add(8, 8, [](std::uint32_t, std::uint32_t) { return 255; });
        const AlphaImage solidMask(solid.describe());
        const AlphaBounds solidBounds(solidMask, sHalf);

        EXPECT_EQ(solidBounds.getWidth(), 8u);
        EXPECT_EQ(solidBounds.getHeight(), 8u);
        EXPECT_EQ(solidBounds.classify(oneTexelAt(0.3f, 0.7f)), Opacity::Opaque);

        StatedMask empty;
        empty.add(8, 8, [](std::uint32_t, std::uint32_t) { return 0; });
        const AlphaImage emptyMask(empty.describe());

        EXPECT_EQ(AlphaBounds(emptyMask, sHalf).classify(oneTexelAt(0.3f, 0.7f)), Opacity::Transparent);
    }

    /// **The whole point of the type, in one comparison.** A mask that is opaque at every texel of
    /// its two finest levels is still not opaque if the level below them is not, because a cone wide
    /// enough reads that level and the cutout answers from it. The two masks here share their finest
    /// level exactly; the second carries one more level, and it is the level that decides.
    ///
    /// A classifier that read only the largest level would call both of these opaque, mark the
    /// microtriangles accordingly, and change the picture wherever the geometry was far enough away
    /// for the cone to reach that level — which is what a canopy seen across a valley is.
    TEST(RtxAlphaBoundsTest, oneCoarserLevelIsEnoughToTakeAVerdictAway)
    {
        StatedMask chain;
        chain.add(4, 4, [](std::uint32_t, std::uint32_t) { return 255; });
        chain.add(2, 2, [](std::uint32_t, std::uint32_t) { return 255; });

        const AlphaImage nearMask(chain.describe());
        ASSERT_EQ(nearMask.getLevelCount(), 2u);
        EXPECT_EQ(AlphaBounds(nearMask, sHalf).classify(oneTexelAt(0.3f, 0.3f)), Opacity::Opaque);

        StatedMask deeper = chain;
        // Forty over 255 is under a sixth, and the cutoff is a half.
        deeper.add(1, 1, [](std::uint32_t, std::uint32_t) { return 40; });

        const AlphaImage farMask(deeper.describe());
        ASSERT_EQ(farMask.getLevelCount(), 3u);
        EXPECT_EQ(AlphaBounds(farMask, sHalf).classify(oneTexelAt(0.3f, 0.3f)), Opacity::Unknown);
    }

    /// A bilinear read reaches past the texel it lands in, and the verdict has to reach with it.
    ///
    /// One hole in an otherwise solid mask. The texel two away is clear of it and stays opaque; the
    /// texel beside it is not, and a bound that compared only the texel a sample lands in would call
    /// it opaque and put a leaf over the hole at every distance.
    TEST(RtxAlphaBoundsTest, theRingABilinearReadCanReachComesOffTheVerdict)
    {
        StatedMask pricked;
        pricked.add(8, 8, [](std::uint32_t x, std::uint32_t y) { return x == 0 && y == 0 ? 0 : 255; });

        const AlphaBounds bounds(AlphaImage(pricked.describe()), sHalf);

        // Texel (2, 2): its three-by-three runs from (1, 1) to (3, 3) and never sees the hole.
        EXPECT_EQ(bounds.classify(oneTexelAt(0.3f, 0.3f)), Opacity::Opaque);

        // Texel (1, 1): its three-by-three starts at the hole.
        EXPECT_EQ(bounds.classify(oneTexelAt(0.2f, 0.2f)), Opacity::Unknown);

        // And the hole itself is not transparent either — everything around it is material.
        EXPECT_EQ(bounds.classify(oneTexelAt(0.05f, 0.05f)), Opacity::Unknown);
    }

    /// A patch outside the unit square wraps into it, because the one sampler this scene shares
    /// repeats — and a coordinate outside it is what a tiling ground or a scrolling banner is.
    TEST(RtxAlphaBoundsTest, aPatchOutsideTheUnitSquareWrapsIntoIt)
    {
        StatedMask split;
        split.add(8, 8, [](std::uint32_t x, std::uint32_t) { return x < 4 ? 255 : 0; });

        const AlphaBounds bounds(AlphaImage(split.describe()), sHalf);

        // Texel two of eight is clear of both seams — the one at four and the one the wrap makes at
        // nought — so it is the one with a verdict to carry round.
        ASSERT_EQ(bounds.classify(oneTexelAt(0.3f, 0.3f)), Opacity::Opaque);
        EXPECT_EQ(bounds.classify(oneTexelAt(1.3f, 1.3f)), Opacity::Opaque);
        EXPECT_EQ(bounds.classify(oneTexelAt(-0.7f, -0.7f)), Opacity::Opaque) << "a negative coordinate wraps "
                                                                                 "forward rather than clamping";

        ASSERT_EQ(bounds.classify(oneTexelAt(0.8f, 0.3f)), Opacity::Transparent);
        EXPECT_EQ(bounds.classify(oneTexelAt(-0.2f, 0.3f)), Opacity::Transparent);
    }

    /// A patch at least as wide as the mask covers all of it, however many times round it goes.
    TEST(RtxAlphaBoundsTest, aPatchAtLeastAsWideAsTheMaskIsTheWholeMask)
    {
        StatedMask split;
        split.add(8, 8, [](std::uint32_t x, std::uint32_t) { return x < 4 ? 255 : 0; });
        const AlphaBounds straddling(AlphaImage(split.describe()), sHalf);

        EXPECT_EQ(straddling.classify(UvBox{ -3.0f, -3.0f, 3.0f, 3.0f }), Opacity::Unknown);

        StatedMask solid;
        solid.add(8, 8, [](std::uint32_t, std::uint32_t) { return 255; });
        const AlphaBounds everywhere(AlphaImage(solid.describe()), sHalf);

        // Counting a texel once per turn would put the count past the area and lose the verdict.
        EXPECT_EQ(everywhere.classify(UvBox{ -3.0f, -3.0f, 3.0f, 3.0f }), Opacity::Opaque);
    }

    /// A mask that could not be decoded has no verdict to give, and inventing one for it would be
    /// deciding on behalf of geometry nothing can answer for.
    TEST(RtxAlphaBoundsTest, aMaskThatCouldNotBeDecodedGivesNoVerdict)
    {
        const AlphaImage nothing{ TextureData{} };
        const AlphaBounds bounds(nothing, sHalf);

        EXPECT_TRUE(bounds.isEmpty());
        EXPECT_EQ(bounds.classify(oneTexelAt(0.5f, 0.5f)), Opacity::Unknown);
    }

    /// A coordinate that is not a number describes no patch, and one that runs away without bound
    /// must not be turned into a texel that overflows the arithmetic under it.
    TEST(RtxAlphaBoundsTest, coordinatesThatNameNoPatchGoOnAsking)
    {
        StatedMask solid;
        solid.add(8, 8, [](std::uint32_t, std::uint32_t) { return 255; });
        const AlphaBounds bounds(AlphaImage(solid.describe()), sHalf);

        const float nowhere = std::numeric_limits<float>::quiet_NaN();
        EXPECT_EQ(bounds.classify(UvBox{ nowhere, nowhere, nowhere, nowhere }), Opacity::Unknown);

        // Far enough out that the texel it lands on does not fit in the type the wrap is done in.
        EXPECT_EQ(bounds.classify(UvBox{ 1e30f, 1e30f, 1e30f, 1e30f }), Opacity::Opaque);
    }
}
