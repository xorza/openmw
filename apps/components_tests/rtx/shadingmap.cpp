#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/shadingmap.hpp>
#include <components/rtx/texturedata.hpp>

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSide = ShadingMap::sExtent;

        /// A linear texture whose texels come from `paint`, so a test can assert its own arithmetic.
        ///
        /// `Rgba8Unorm` is the format that exists for this: it is the one the renderer treats as
        /// linear, so what goes in is what the luminance is computed from and no transfer function
        /// stands between the expectation and the answer.
        struct Painted
        {
            std::vector<std::byte> mBytes;
            MipLevel mLevel;

            template <class Paint>
            Painted(std::uint32_t width, std::uint32_t height, Paint&& paint)
                : mBytes(std::size_t{ width } * height * 4)
                , mLevel{ 0, width, height }
            {
                for (std::uint32_t y = 0; y < height; ++y)
                    for (std::uint32_t x = 0; x < width; ++x)
                    {
                        const std::uint8_t grey = paint(x, y);
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            mBytes[(std::size_t{ y } * width + x) * 4 + channel] = std::byte{ grey };

                        mBytes[(std::size_t{ y } * width + x) * 4 + 3] = std::byte{ 255 };
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

        /// The largest distance any cell is from one, which is how far a map is from neutral.
        float furthestFromNeutral(const ShadingMap& map)
        {
            float furthest = 0.0f;
            for (const float value : map.getValues())
                furthest = std::max(furthest, std::abs(value - 1.0f));

            return furthest;
        }

        float meanOf(const ShadingMap& map)
        {
            float sum = 0.0f;
            for (const float value : map.getValues())
                sum += value;

            return sum / static_cast<float>(map.getValues().size());
        }

        /// A texture with nothing painted into it has nothing to take out of it.
        TEST(RtxShadingMapTest, anEvenTextureComesBackNeutral)
        {
            const Painted flat(256, 256, [](std::uint32_t, std::uint32_t) { return std::uint8_t{ 137 }; });

            EXPECT_LT(furthestFromNeutral(ShadingMap(flat.describe())), 0.01f);
        }

        /// Detail alternating every texel is not lighting, and a map that followed it would flatten
        /// the texture into a colour.
        ///
        /// **This is the test the whole grid size exists for.** A checkerboard has the widest
        /// possible swing between neighbouring texels and no low-frequency content at all, so an
        /// estimate that answers anything but neutral here is reading paint as light.
        TEST(RtxShadingMapTest, aCheckerboardIsLeftAlone)
        {
            const Painted checks(256, 256,
                [](std::uint32_t x, std::uint32_t y) { return static_cast<std::uint8_t>((x + y) % 2 ? 20 : 220); });

            EXPECT_LT(furthestFromNeutral(ShadingMap(checks.describe())), 0.01f);
        }

        /// A painted gradient is what lighting looks like, and the map has to find it.
        ///
        /// **A cosine and not a ramp, because the blur wraps.** Morrowind's textures tile and the
        /// estimate blurs accordingly, so a gradient that does not meet itself at the edges is an
        /// input no tiling texture can present — a straight ramp would have its two ends averaged
        /// together and the test would be measuring that rather than the estimate.
        ///
        /// The arithmetic, end to end. The texture is `120 + 60 cos(2 pi x / 256)` in linear, which
        /// is 60 to 180 and three to one. One period spans the grid's thirty-two cells, so each of
        /// the three box blurs keeps `(1 + 2 cos(2 pi / 32)) / 3 = 0.98719` of the amplitude and
        /// three keep 0.96206 of it. Normalising divides by the mean, 120, so the map is
        /// `1 + 0.96206 * 0.5 * cos`, running 0.519 to 1.481 — a ratio of 2.854, which is what a
        /// three-to-one gradient survives as and comfortably inside the clamps.
        TEST(RtxShadingMapTest, aPaintedGradientIsRecoveredAtTheStrengthItWasPainted)
        {
            const Painted lit(256, 256, [](std::uint32_t x, std::uint32_t) {
                const double angle = 2.0 * std::numbers::pi * x / 256.0;
                return static_cast<std::uint8_t>(std::lround(120.0 + 60.0 * std::cos(angle)));
            });

            const ShadingMap map(lit.describe());
            const std::span<const float> values = map.getValues();

            EXPECT_NEAR(meanOf(map), 1.0f, 0.01f) << "the estimate moves light rather than adding it";

            // The middle row: the texture does not vary down the image, so every row is the same
            // and this one is as good as any.
            const std::size_t row = std::size_t{ sSide / 2 } * sSide;
            const auto brightest = std::max_element(values.begin() + row, values.begin() + row + sSide);
            const auto darkest = std::min_element(values.begin() + row, values.begin() + row + sSide);

            EXPECT_NEAR(*brightest, 1.481f, 0.02f);
            EXPECT_NEAR(*darkest, 0.519f, 0.02f);
            EXPECT_NEAR(*brightest / *darkest, 2.854f, 0.1f) << "three to one, less what the blur takes";

            // And in the right places: the texture peaks at its left edge and troughs halfway
            // across, so a map that was inverted or shifted would be caught here and nowhere else.
            EXPECT_EQ(std::distance(values.begin() + row, brightest), 0);
            EXPECT_EQ(std::distance(values.begin() + row, darkest), sSide / 2);
        }

        /// The clamps, on a texture that swings further than any lighting would.
        TEST(RtxShadingMapTest, theCorrectionIsBoundedBothWays)
        {
            // Black for the left half and white for the right: a hundred to one, which is paint and
            // not shadow, and exactly what the clamps are there to survive.
            const Painted halves(
                256, 256, [](std::uint32_t x, std::uint32_t) { return static_cast<std::uint8_t>(x < 128 ? 3 : 250); });

            const ShadingMap map(halves.describe());
            for (const float value : map.getValues())
            {
                EXPECT_GE(value, ShadingMap::sFloor);
                EXPECT_LE(value, ShadingMap::sCeiling);
            }

            EXPECT_NEAR(map.getValues()[std::size_t{ sSide / 2 } * sSide + 1], ShadingMap::sFloor, 0.001f)
                << "the black half is at the floor";
        }

        /// The compressed path, which is the one every texture in the game actually takes.
        ///
        /// **A block's mean is its palette weighted by how many texels chose each entry**, and this
        /// is where that arithmetic is checked — endpoint decoding, four-colour mode, index
        /// unpacking and the transfer function, none of which the linear tests above can reach.
        ///
        /// Two endpoints: `0xFFFF` is white, and `0x8410` is five-six-five `(16, 32, 16)`, which
        /// replicates to bytes `(132, 130, 132)`. Through the sRGB curve those are 0.23074, 0.22323
        /// and 0.23074 linear, and Rec. 709 weights them to a luminance of 0.225369 — against
        /// 0.5117 if the bytes were read as if they were already linear, which is the mistake this
        /// pins. White is 1.0 either way, so the endpoints alone would not have caught it.
        ///
        /// The left half's blocks are eight texels of each endpoint, so their mean is 0.612685; the
        /// right half's are two and fourteen, giving 0.322198. The texture's mean is halfway between
        /// at 0.467441, so the map has to come back 1.3108 on the left and 0.6893 on the right.
        TEST(RtxShadingMapTest, aCompressedBlockAveragesItsPaletteByTheIndicesThatChoseIt)
        {
            constexpr std::uint32_t side = 256;
            constexpr std::uint32_t blocks = side / 4;

            std::vector<std::byte> bytes(std::size_t{ blocks } * blocks * 8);
            for (std::uint32_t row = 0; row < blocks; ++row)
                for (std::uint32_t column = 0; column < blocks; ++column)
                {
                    const std::size_t at = (std::size_t{ row } * blocks + column) * 8;

                    // c0 first and larger, which is what says four colours rather than three.
                    bytes[at + 0] = std::byte{ 0xFF };
                    bytes[at + 1] = std::byte{ 0xFF };
                    bytes[at + 2] = std::byte{ 0x10 };
                    bytes[at + 3] = std::byte{ 0x84 };

                    const bool left = column < blocks / 2;
                    for (std::size_t line = 0; line < 4; ++line)
                    {
                        // Two bits a texel, lowest first. 0x00 is four of the first endpoint, 0x55
                        // is four of the second, and 0x50 is two of each.
                        const std::uint8_t indices = left ? (line < 2 ? 0x00 : 0x55) : (line == 0 ? 0x50 : 0x55);
                        bytes[at + 4 + line] = std::byte{ indices };
                    }
                }

            const MipLevel level{ 0, side, side };
            const TextureData texture{
                .mFormat = TextureFormat::Bc1RgbaSrgb,
                .mWidth = side,
                .mHeight = side,
                .mBytes = bytes,
                .mLevels = std::span(&level, 1),
            };

            const ShadingMap map(texture);
            const std::span<const float> values = map.getValues();
            const std::size_t row = std::size_t{ sSide / 2 } * sSide;

            // A quarter and three quarters across, which is as far from both the step and the wrap
            // as a cell gets.
            EXPECT_NEAR(values[row + sSide / 4], 1.3108f, 0.01f) << "the brighter half";
            EXPECT_NEAR(values[row + 3 * sSide / 4], 0.6893f, 0.01f) << "and the darker one";
        }

        /// A texture too small to fill the grid, which most of Morrowind's smaller ones are.
        ///
        /// **Sixty-four texels land in sixty-four cells and leave nine hundred and sixty empty.**
        /// Reading those as black would make the estimate a spike wherever the texture did reach,
        /// and drive the correction straight into its clamps — a flat texture would come back as a
        /// grid of light and dark squares and paint them onto every surface using it. Too few
        /// texels to resolve shading has to mean the same as having no shading.
        TEST(RtxShadingMapTest, aTextureSmallerThanTheGridStillComesBackNeutral)
        {
            const Painted small(8, 8, [](std::uint32_t, std::uint32_t) { return std::uint8_t{ 90 }; });

            EXPECT_LT(furthestFromNeutral(ShadingMap(small.describe())), 0.01f);
        }

        /// A map for a texture that would not load, which is the one every missing material gets.
        TEST(RtxShadingMapTest, theDefaultMapChangesNothing)
        {
            const ShadingMap neutral;

            EXPECT_EQ(neutral.getValues().size(), std::size_t{ sSide } * sSide);
            for (const float value : neutral.getValues())
                EXPECT_EQ(value, 1.0f);
        }
    }
}
