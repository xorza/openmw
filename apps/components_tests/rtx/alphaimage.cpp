#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/alphaimage.hpp>
#include <components/rtx/texturedata.hpp>

namespace
{
    using namespace Rtx;

    /// A texture of one block, described the way `TextureBuilder` hands one over.
    struct OneBlock
    {
        std::vector<std::byte> mBytes;
        MipLevel mLevel{ 0, 4, 4 };
        TextureFormat mFormat;

        OneBlock(TextureFormat format, std::initializer_list<std::uint8_t> bytes)
            : mFormat(format)
        {
            for (const std::uint8_t byte : bytes)
                mBytes.push_back(std::byte{ byte });
        }

        TextureData describe() const
        {
            return TextureData{
                .mFormat = mFormat,
                .mWidth = 4,
                .mHeight = 4,
                .mBytes = mBytes,
                .mLevels = std::span(&mLevel, 1),
            };
        }
    };

    /// BC2 states alpha outright: four bits a texel, widened so that fifteen is opaque.
    ///
    /// The nibbles below run 0, 1, 2 … 15 across the block in texel order, so every one of the
    /// sixteen values is asserted at once and a decoder that swapped the two halves of a byte or
    /// walked the texels in column order fails on the second texel.
    TEST(RtxAlphaImageTest, bc2StatesFourBitsATexelWidenedSoFifteenIsOpaque)
    {
        // Texel 0 in the low nibble of the first byte, texel 1 in its high nibble.
        const OneBlock block(
            TextureFormat::Bc2Srgb, { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0 });

        const AlphaImage alpha(block.describe());
        ASSERT_EQ(alpha.getWidth(), 4u);

        for (std::uint32_t texel = 0; texel < 16; ++texel)
        {
            // Seventeen and not sixteen: fifteen has to land on 255, or nothing is ever opaque.
            const auto expected = static_cast<std::uint8_t>(texel * 17);
            EXPECT_EQ(alpha.at(0, texel % 4, texel / 4), expected) << "texel " << texel;
        }

        EXPECT_EQ(alpha.at(0, 3, 3), 255) << "the last nibble is fifteen and must be fully opaque";
    }

    /// BC3 builds a palette from two endpoints, and which palette depends on their order.
    ///
    /// **Both spellings, because a decoder that assumed one reads half the blocks wrong.** With the
    /// first endpoint the larger, all eight entries are interpolated between them; with it smaller,
    /// six are and the last two are nought and full outright — which is the spelling a texture with
    /// hard cutout edges is compressed into.
    TEST(RtxAlphaImageTest, bc3InterpolatesEightWaysDescendingAndSixWithTheEndsAscending)
    {
        // Indices are three bits each, little-endian over six bytes. All zero picks endpoint one,
        // so the first texel is the first endpoint in both spellings below.
        const OneBlock descending(
            TextureFormat::Bc3Srgb, { 255, 0, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0, 0, 0, 0, 0, 0, 0, 0 });
        const OneBlock ascending(
            TextureFormat::Bc3Srgb, { 0, 255, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0, 0, 0, 0, 0, 0, 0, 0 });

        // 0x888888888888 taken three bits at a time from the bottom gives indices 0, 1, 2, 4
        // repeating, which reaches four of the eight entries without hand-packing all sixteen.
        const AlphaImage high(descending.describe());
        const AlphaImage low(ascending.describe());

        EXPECT_EQ(high.at(0, 0, 0), 255) << "index nought is the first endpoint";
        EXPECT_EQ(low.at(0, 0, 0), 0);

        // Entry one is the second endpoint under both spellings.
        EXPECT_EQ(high.at(0, 1, 0), 0);
        EXPECT_EQ(low.at(0, 1, 0), 255);

        // Entry two is where the two palettes part: descending divides the span into sevenths and
        // ascending into fifths, so (6*255)/7 = 218 against (1*255)/5 = 51.
        EXPECT_EQ(high.at(0, 2, 0), 218);
        EXPECT_EQ(low.at(0, 2, 0), 51);

        // And entry four, further along the same two ramps: (4*255)/7 = 145 against (3*255)/5 = 153.
        EXPECT_EQ(high.at(0, 3, 0), 145);
        EXPECT_EQ(low.at(0, 3, 0), 153);

        // **The last two entries are what the ascending spelling is for**, and the case a cutout
        // texture is compressed into: they are nought and full outright rather than interpolated,
        // so a hard edge survives the block. Indices six and seven, packed into the first two
        // texels: 0b111'110 is 0x3E.
        const OneBlock ends(TextureFormat::Bc3Srgb, { 0, 255, 0x3E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        const OneBlock ramp(TextureFormat::Bc3Srgb, { 255, 0, 0x3E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });

        const AlphaImage terminal(ends.describe());
        EXPECT_EQ(terminal.at(0, 0, 0), 0) << "entry six is nothing at all, not an interpolated step";
        EXPECT_EQ(terminal.at(0, 1, 0), 255) << "and entry seven is fully opaque";

        // The same indices under the descending spelling are ordinary steps of the ramp, which is
        // what says the two palettes really are different tables and not one with a flag on it.
        const AlphaImage stepped(ramp.describe());
        EXPECT_EQ(stepped.at(0, 0, 0), 72);
        EXPECT_EQ(stepped.at(0, 1, 0), 36);
    }

    /// BC1 has no alpha channel at all — it has a fourth palette entry meaning "nothing here", and
    /// only when the endpoints are stored ascending.
    TEST(RtxAlphaImageTest, bc1IsCutoutOrNothingAndOnlyWhenItsEndpointsAscend)
    {
        // Endpoints 0x0000 then 0xFFFF: ascending, so index three is the transparent entry.
        const OneBlock cutout(TextureFormat::Bc1RgbaSrgb, { 0x00, 0x00, 0xFF, 0xFF, 0xE4, 0, 0, 0 });

        // The same block with the endpoints the other way round is opaque throughout, index three
        // included — the bits did not move, only what they mean.
        const OneBlock opaque(TextureFormat::Bc1RgbaSrgb, { 0xFF, 0xFF, 0x00, 0x00, 0xE4, 0, 0, 0 });

        const AlphaImage cut(cutout.describe());
        const AlphaImage solid(opaque.describe());

        // 0xE4 is 11 10 01 00: texels 0..3 take indices 0, 1, 2, 3.
        EXPECT_EQ(cut.at(0, 0, 0), 255);
        EXPECT_EQ(cut.at(0, 1, 0), 255);
        EXPECT_EQ(cut.at(0, 2, 0), 255);
        EXPECT_EQ(cut.at(0, 3, 0), 0) << "index three is the hole, and only in the ascending spelling";

        for (std::uint32_t x = 0; x < 4; ++x)
            EXPECT_EQ(solid.at(0, x, 0), 255) << "descending endpoints spend index three on a colour";
    }

    /// Uncompressed textures state alpha as the fourth byte whichever order the three colours are in.
    TEST(RtxAlphaImageTest, theUncompressedSpellingsTakeAlphaFromTheFourthByte)
    {
        for (const TextureFormat format :
            { TextureFormat::Rgba8Unorm, TextureFormat::Rgba8Srgb, TextureFormat::Bgra8Srgb })
        {
            std::vector<std::byte> bytes(4 * 4 * 4, std::byte{ 0 });
            for (std::uint32_t texel = 0; texel < 16; ++texel)
                bytes[texel * 4 + 3] = std::byte{ static_cast<std::uint8_t>(texel * 16) };

            const MipLevel level{ 0, 4, 4 };
            const TextureData texture{
                .mFormat = format,
                .mWidth = 4,
                .mHeight = 4,
                .mBytes = bytes,
                .mLevels = std::span(&level, 1),
            };

            const AlphaImage alpha(texture);
            for (std::uint32_t texel = 0; texel < 16; ++texel)
                EXPECT_EQ(alpha.at(0, texel % 4, texel / 4), static_cast<std::uint8_t>(texel * 16))
                    << "texel " << texel << " of format " << static_cast<int>(format);
        }
    }

    /// The whole mip chain is decoded, because the whole mip chain is what the shader can read.
    ///
    /// `alphaPasses` samples the mask at the level the ray's cone resolves to, so a classifier that
    /// saw only the largest level would be bounding a fetch that never happens. The two levels below
    /// disagree outright — a level that was read from the wrong offset, or skipped, comes back with
    /// the other one's values.
    TEST(RtxAlphaImageTest, everyLevelTheFileCarriesIsDecodedAndNotOnlyTheLargest)
    {
        // Four by four at 40, then two by two at 200. A real chain averages; these do not, so that
        // reading the wrong level is a failure rather than a rounding difference.
        std::vector<std::byte> bytes(4 * 4 * 4 + 2 * 2 * 4, std::byte{ 0 });
        for (std::size_t texel = 0; texel < 16; ++texel)
            bytes[texel * 4 + 3] = std::byte{ 40 };
        for (std::size_t texel = 0; texel < 4; ++texel)
            bytes[4 * 4 * 4 + texel * 4 + 3] = std::byte{ 200 };

        const std::array<MipLevel, 2> levels{ MipLevel{ 0, 4, 4 }, MipLevel{ 4 * 4 * 4, 2, 2 } };
        const TextureData texture{
            .mFormat = TextureFormat::Rgba8Unorm,
            .mWidth = 4,
            .mHeight = 4,
            .mBytes = bytes,
            .mLevels = levels,
        };

        const AlphaImage alpha(texture);

        ASSERT_EQ(alpha.getLevelCount(), 2u);
        EXPECT_EQ(alpha.getWidth(), 4u);
        EXPECT_EQ(alpha.getHeight(), 4u);
        EXPECT_EQ(alpha.getLevel(1).mWidth, 2u);
        EXPECT_EQ(alpha.getLevel(1).mHeight, 2u);

        for (std::uint32_t texel = 0; texel < 16; ++texel)
            EXPECT_EQ(alpha.at(0, texel % 4, texel / 4), 40) << "texel " << texel << " of the largest level";
        for (std::uint32_t texel = 0; texel < 4; ++texel)
            EXPECT_EQ(alpha.at(1, texel % 2, texel / 2), 200) << "texel " << texel << " of the level below it";
    }

    /// A texture with no levels is one whose cutout could not be read, and nothing is invented for
    /// it: a caller that cannot answer for a mask has to leave its geometry asking.
    TEST(RtxAlphaImageTest, aTextureWithNothingInItDecodesToNothingAtAll)
    {
        const TextureData nothing{};
        const AlphaImage alpha(nothing);

        EXPECT_TRUE(alpha.isEmpty());
        EXPECT_EQ(alpha.getLevelCount(), 0u);
        EXPECT_EQ(alpha.getWidth(), 0u);
    }
}
