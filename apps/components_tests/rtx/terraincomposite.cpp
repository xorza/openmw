#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/shadingmap.hpp>
#include <components/rtx/terraincomposite.hpp>
#include <components/rtx/texturedata.hpp>

namespace
{
    using namespace Rtx;

    /// A loose `Rgba8Srgb` texture of one level, described the way the uploader is handed one.
    ///
    /// Uncompressed on purpose: a block cannot state an arbitrary colour, and every expectation
    /// below is an exact one.
    struct Flat
    {
        std::vector<std::byte> mBytes;
        MipLevel mLevel;

        /// One `0xRRGGBB` a texel, row by row from the top left.
        Flat(std::uint32_t side, const std::vector<std::uint32_t>& texels)
            : mLevel{ 0, side, side }
        {
            EXPECT_EQ(texels.size(), std::size_t{ side } * side) << "a texture that is not as big as it says";

            mBytes.reserve(texels.size() * 4);
            for (const std::uint32_t colour : texels)
            {
                mBytes.push_back(std::byte{ static_cast<std::uint8_t>(colour >> 16) });
                mBytes.push_back(std::byte{ static_cast<std::uint8_t>(colour >> 8) });
                mBytes.push_back(std::byte{ static_cast<std::uint8_t>(colour) });
                mBytes.push_back(std::byte{ 255 });
            }
        }

        TextureData describe() const
        {
            return TextureData{
                .mFormat = TextureFormat::Rgba8Srgb,
                .mWidth = mLevel.mWidth,
                .mHeight = mLevel.mHeight,
                .mBytes = mBytes,
                .mLevels = std::span(&mLevel, 1),
            };
        }
    };

    std::vector<std::uint32_t> filled(std::uint32_t side, std::uint32_t colour)
    {
        return std::vector<std::uint32_t>(std::size_t{ side } * side, colour);
    }

    /// One texel of a baked level, as `0xRRGGBB`.
    std::uint32_t texelOf(const TextureData& baked, std::uint32_t level, std::uint32_t x, std::uint32_t y)
    {
        const MipLevel& which = baked.mLevels[level];
        const std::size_t at = which.mOffset + (std::size_t{ y } * which.mWidth + x) * 4;
        const auto channel
            = [&](std::size_t offset) { return std::to_integer<std::uint32_t>(baked.mBytes[at + offset]); };

        return channel(0) << 16 | channel(1) << 8 | channel(2);
    }

    /// Two ground types, each showing at exactly half weight over the whole chunk.
    ///
    /// **In light, half of one and half of the other is 0.5 a channel**, which sRGB states as
    /// `1.055 * 0.5^(1/2.4) - 0.055 = 0.7354`, and `0.7354 * 255 = 187.5`, so 188. Summing the
    /// stored bytes instead gives 128 — the whole difference between ground that blends where two
    /// types meet and ground that goes muddy there.
    TEST(RtxTerrainCompositeTest, aStackIsSummedInLightAndNotInStoredBytes)
    {
        const Flat red(1, filled(1, 0xFF0000));
        const Flat green(1, filled(1, 0x00FF00));
        const std::array<float, 1> half{ 0.5f };

        const std::array layers{
            CompositeLayer{ .mDiffuse = red.describe(), .mMask = half, .mMaskWidth = 1, .mMaskHeight = 1 },
            CompositeLayer{ .mDiffuse = green.describe(), .mMask = half, .mMaskWidth = 1, .mMaskHeight = 1 },
        };

        const TerrainComposite baked(layers, 4, 0.0f);
        const TextureData described = baked.describe();

        ASSERT_EQ(described.mWidth, 4u);
        ASSERT_EQ(described.mFormat, TextureFormat::Rgba8Srgb);

        for (std::uint32_t y = 0; y < 4; ++y)
            for (std::uint32_t x = 0; x < 4; ++x)
                EXPECT_EQ(texelOf(described, 0, x, y), 0xBCBC00u) << "at " << x << ", " << y;

        // Ground is not see-through, and a composite that left its alpha at nought is a chunk the
        // cutout test throws every ray straight through.
        EXPECT_EQ(std::to_integer<std::uint32_t>(described.mBytes[3]), 255u);

        // A composite has no file and so no estimate of its own to make: the light painted into each
        // ground texture came off per tile in the bake, which is the only place the tiling is known.
        EXPECT_EQ(described.mShading.size(), std::size_t{ ShadingMap::sExtent } * ShadingMap::sExtent);
        for (const float factor : described.mShading)
            EXPECT_FLOAT_EQ(factor, 1.0f) << "a composite that would be corrected a second time";
    }

    /// The masks decide which ground type is on which side, and the chain averages them in light.
    ///
    /// Two masks two texels across, `{1, 0}` and `{0, 1}`, over a composite four across. Texel
    /// centres land at `u = 0.125, 0.375, 0.625, 0.875`; against a two-texel grid whose own centres
    /// are at 0.25 and 0.75 that is `u * 2 - 0.5 = -0.25, 0.25, 0.75, 1.25`, which clamps and
    /// interpolates to weights of **1, 0.75, 0.25, 0** for the first layer and the reverse for the
    /// second. A bake that read the mask backwards puts the ground types on the wrong sides and
    /// nothing but an exact expectation says so.
    TEST(RtxTerrainCompositeTest, theMaskPlacesEachGroundTypeAndTheChainAveragesThemInLight)
    {
        const Flat red(1, filled(1, 0xFF0000));
        const Flat green(1, filled(1, 0x00FF00));
        const std::array<float, 2> west{ 1.0f, 0.0f };
        const std::array<float, 2> east{ 0.0f, 1.0f };

        const std::array layers{
            CompositeLayer{ .mDiffuse = red.describe(), .mMask = west, .mMaskWidth = 2, .mMaskHeight = 1 },
            CompositeLayer{ .mDiffuse = green.describe(), .mMask = east, .mMaskWidth = 2, .mMaskHeight = 1 },
        };

        const TerrainComposite baked(layers, 4, 0.0f);
        const TextureData described = baked.describe();

        // 0.75 encodes as 224.6 → 225 = 0xE1, and 0.25 as 137.0 → 137 = 0x89.
        const std::array<std::uint32_t, 4> expected{ 0xFF0000u, 0xE18900u, 0x89E100u, 0x00FF00u };
        for (std::uint32_t y = 0; y < 4; ++y)
            for (std::uint32_t x = 0; x < 4; ++x)
                EXPECT_EQ(texelOf(described, 0, x, y), expected[x]) << "at " << x << ", " << y;

        // Four, two, one — a chain that stops early is a distant chunk sampling its finest level and
        // shimmering as the camera turns, which is the one thing distance was supposed to fix.
        ASSERT_EQ(baked.getLevelCount(), 3u);

        // The pairs average in light before they are re-encoded: (1 + 0.75) / 2 = 0.875 → 240 = 0xF0
        // and (0 + 0.25) / 2 = 0.125 → 99 = 0x63. Averaging the bytes gives 240 and 68.
        EXPECT_EQ(texelOf(described, 1, 0, 0), 0xF06300u);
        EXPECT_EQ(texelOf(described, 1, 1, 0), 0x63F000u);

        // And the last level is the whole chunk in one texel: 0.5 of each, which is 188 again.
        EXPECT_EQ(texelOf(described, 2, 0, 0), 0xBCBC00u);
    }

    /// The diffuse transform is where the ground's tiling lives, and the sampler under it repeats.
    ///
    /// At a composite as wide as the texture and a transform of one, a texel centre lands exactly on
    /// a texel centre — so the bake is a copy, and a transpose or a flipped row shows up at once.
    /// Offsetting by a quarter is exactly one texel of a four-wide texture, so the same bake comes
    /// back rolled by a column, with the last wrapping around to the first.
    TEST(RtxTerrainCompositeTest, anIdentityTransformCopiesTexelsAndAnOffsetRollsThemAround)
    {
        const std::array<std::uint32_t, 4> columns{ 0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFFFFu };

        std::vector<std::uint32_t> texels;
        for (std::uint32_t y = 0; y < 4; ++y)
            for (std::uint32_t x = 0; x < 4; ++x)
                texels.push_back(columns[x]);

        const Flat ground(4, texels);

        const std::array straight{ CompositeLayer{ .mDiffuse = ground.describe() } };
        const std::array rolled{ CompositeLayer{
            .mDiffuse = ground.describe(), .mDiffuseTransform = osg::Vec4f(1.0f, 1.0f, 0.25f, 0.0f) } };

        const TerrainComposite asIs(straight, 4, 0.0f);
        const TerrainComposite shifted(rolled, 4, 0.0f);
        const TextureData copied = asIs.describe();
        const TextureData moved = shifted.describe();

        for (std::uint32_t y = 0; y < 4; ++y)
            for (std::uint32_t x = 0; x < 4; ++x)
            {
                EXPECT_EQ(texelOf(copied, 0, x, y), columns[x]) << "at " << x << ", " << y;
                EXPECT_EQ(texelOf(moved, 0, x, y), columns[(x + 1) % 4]) << "at " << x << ", " << y;
            }
    }

    /// The light painted into a ground texture is divided out as far as the strength says.
    ///
    /// A map of two everywhere, against white: in light that is 1.0 divided by `mix(1, 2, delight)`.
    /// None of it leaves white alone; all of it halves the layer to 0.5, which is 188; half of it
    /// divides by 1.5, and `1 / 1.5 = 0.6667` encodes as 213.2 → 213. Three points because two would
    /// not say whether the strength is read at all or merely switched on.
    TEST(RtxTerrainCompositeTest, theLightPaintedIntoALayerIsDividedOutAsFarAsTheStrengthSays)
    {
        const Flat white(1, filled(1, 0xFFFFFF));
        const std::vector<float> twice(std::size_t{ ShadingMap::sExtent } * ShadingMap::sExtent, 2.0f);
        const std::array layers{ CompositeLayer{ .mDiffuse = white.describe(), .mShading = twice } };

        const TerrainComposite untouched(layers, 2, 0.0f);
        const TerrainComposite halved(layers, 2, 1.0f);
        const TerrainComposite partly(layers, 2, 0.5f);

        EXPECT_EQ(texelOf(untouched.describe(), 0, 0, 0), 0xFFFFFFu);
        EXPECT_EQ(texelOf(halved.describe(), 0, 0, 0), 0xBCBCBCu);
        EXPECT_EQ(texelOf(partly.describe(), 0, 0, 0), 0xD5D5D5u);
    }
}
