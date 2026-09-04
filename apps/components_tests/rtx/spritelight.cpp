#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/alphaimage.hpp>
#include <components/rtx/spritelight.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/vfs/pathutil.hpp>

namespace Rtx
{
    namespace
    {
        /// An uncompressed texture whose levels' alphas a test states outright, with colour it ignores.
        struct AlphaSheet
        {
            std::vector<std::byte> mBytes;
            std::vector<MipLevel> mLevels;

            /// One level of `width` by `height`, with `alphas` in row order.
            AlphaSheet(std::uint32_t width, std::uint32_t height, std::initializer_list<std::uint8_t> alphas)
            {
                addLevel(width, height, alphas);
            }

            void addLevel(std::uint32_t width, std::uint32_t height, std::initializer_list<std::uint8_t> alphas)
            {
                mLevels.push_back(MipLevel{ static_cast<std::uint32_t>(mBytes.size()), width, height });
                for (const std::uint8_t alpha : alphas)
                {
                    mBytes.insert(mBytes.end(), 3, std::byte{ 255 });
                    mBytes.push_back(std::byte{ alpha });
                }
            }

            TextureData describe() const
            {
                return TextureData{
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = mLevels.front().mWidth,
                    .mHeight = mLevels.front().mHeight,
                    .mBytes = mBytes,
                    .mLevels = mLevels,
                };
            }
        };

        /// Light crossing a row is thinned by every texel beyond the one it reaches, and by none before.
        ///
        /// A row of four with alpha 128 in its middle two texels. Each of those keeps
        /// `(1 - 128/255)^(1/4) = 0.49804^0.25 = 0.84007` of what crosses it, so the texel at the edge
        /// away from the light sees both — `0.84007^2 = 0.70572`, byte 180 — the next one in sees one,
        /// byte 214, and the two on the light's own side see nothing in the way. `+u` is light from the
        /// high-`x` edge and `-u` from the low, so each channel is the other's mirror. The row is one
        /// texel high, so the two `v` channels have nothing to cross at all.
        ///
        /// **And the same numbers stand a column up**, which is what pins which channel is which axis:
        /// a bake that swapped `u` and `v` would put the row's shadows into the column's channels.
        TEST(RtxSpriteLightMapTest, aTexelIsShadowedByWhatLiesBetweenItAndTheEdgeTheLightIsOn)
        {
            constexpr std::array<std::uint8_t, 4> fromHigh{ 180, 214, 255, 255 };
            constexpr std::array<std::uint8_t, 4> fromLow{ 255, 255, 214, 180 };

            const AlphaSheet row(4, 1, { 0, 128, 128, 0 });
            const AlphaImage rowAlpha(row.describe());
            const SpriteLightMap acrossRow(rowAlpha);
            ASSERT_FALSE(acrossRow.isEmpty());

            for (std::uint32_t x = 0; x < 4; ++x)
            {
                EXPECT_EQ(acrossRow.at(0, x, 0, 0), fromHigh[x]) << "+u at " << x;
                EXPECT_EQ(acrossRow.at(0, x, 0, 1), fromLow[x]) << "-u at " << x;
                EXPECT_EQ(acrossRow.at(0, x, 0, 2), 255) << "+v at " << x;
                EXPECT_EQ(acrossRow.at(0, x, 0, 3), 255) << "-v at " << x;
            }

            const AlphaSheet column(1, 4, { 0, 128, 128, 0 });
            const AlphaImage columnAlpha(column.describe());
            const SpriteLightMap downColumn(columnAlpha);

            for (std::uint32_t y = 0; y < 4; ++y)
            {
                EXPECT_EQ(downColumn.at(0, 0, y, 0), 255) << "+u at " << y;
                EXPECT_EQ(downColumn.at(0, 0, y, 1), 255) << "-u at " << y;
                EXPECT_EQ(downColumn.at(0, 0, y, 2), fromHigh[y]) << "+v at " << y;
                EXPECT_EQ(downColumn.at(0, 0, y, 3), fromLow[y]) << "-v at " << y;
            }
        }

        /// An opaque texel stops the light outright, and only for what lies beyond it.
        ///
        /// `(1 - 1)^(1/4)` is nought whatever the power, so everything past an opaque texel from the
        /// light's side is dark, and the texel itself and everything before it are untouched.
        TEST(RtxSpriteLightMapTest, anOpaqueTexelStopsTheLightForEverythingBeyondIt)
        {
            const AlphaSheet row(4, 1, { 0, 255, 0, 0 });
            const AlphaImage alpha(row.describe());
            const SpriteLightMap map(alpha);

            constexpr std::array<std::uint8_t, 4> fromHigh{ 0, 255, 255, 255 };
            constexpr std::array<std::uint8_t, 4> fromLow{ 255, 255, 0, 0 };
            for (std::uint32_t x = 0; x < 4; ++x)
            {
                EXPECT_EQ(map.at(0, x, 0, 0), fromHigh[x]) << "+u at " << x;
                EXPECT_EQ(map.at(0, x, 0, 1), fromLow[x]) << "-u at " << x;
            }
        }

        /// Every level is baked from its own alpha and laid out back to back, the way an upload reads
        /// them.
        ///
        /// A two-by-two level with one alpha-128 texel and the one-by-one level under it. On the top
        /// level the texel at `(0, 0)` sees the 128 at `(1, 0)` from `+u` — `0.49804^(1/2) = 0.70572`,
        /// byte 180 — and from `+v` sees the blank at `(0, 1)`; the bottom level has nothing to cross
        /// in any direction. The second level starts sixteen bytes in, after the first's four texels.
        TEST(RtxSpriteLightMapTest, levelsAreBakedApartAndDescribedBackToBack)
        {
            AlphaSheet sheet(2, 2, { 0, 128, 0, 0 });
            sheet.addLevel(1, 1, { 32 });

            const AlphaImage alpha(sheet.describe());
            ASSERT_EQ(alpha.getLevelCount(), 2u);
            const SpriteLightMap map(alpha);

            const TextureData described = map.describe();
            EXPECT_EQ(described.mFormat, TextureFormat::Rgba8Unorm);
            EXPECT_EQ(described.mWidth, 2u);
            EXPECT_EQ(described.mHeight, 2u);
            ASSERT_EQ(described.mLevels.size(), 2u);
            EXPECT_EQ(described.mLevels[0].mOffset, 0u);
            EXPECT_EQ(described.mLevels[1].mOffset, 16u);
            EXPECT_EQ(described.mLevels[1].mWidth, 1u);
            EXPECT_EQ(described.mBytes.size(), 20u);

            EXPECT_EQ(map.at(0, 0, 0, 0), 180) << "+u across the 128";
            EXPECT_EQ(map.at(0, 0, 0, 2), 255) << "+v across a blank";
            EXPECT_EQ(map.at(0, 1, 0, 1), 255) << "-u from the blank side";
            EXPECT_EQ(map.at(0, 1, 1, 3), 180) << "-v up through the 128";

            for (std::uint32_t channel = 0; channel < 4; ++channel)
                EXPECT_EQ(map.at(1, 0, 0, channel), 255) << "the one-texel level, channel " << channel;

            // The description's bytes are the map's own, in the layout `at` reads.
            EXPECT_EQ(static_cast<std::uint8_t>(described.mBytes[0]), 180);
            EXPECT_EQ(static_cast<std::uint8_t>(described.mBytes[16]), 255);
        }

        /// A bake's key names its source and nothing else's key does.
        TEST(RtxSpriteLightMapTest, theKeyNamesTheSourceAndOtherBakesAreNotMistakenForOne)
        {
            const VFS::Path::NormalizedView source("textures/tx_smoke.dds");
            const std::string key = SpriteLightMap::keyFor(source);
            EXPECT_EQ(key, "sprite/textures/tx_smoke.dds");

            const std::optional<VFS::Path::Normalized> found = SpriteLightMap::sourceOf(key);
            ASSERT_TRUE(found.has_value());
            EXPECT_EQ(*found, source);

            EXPECT_FALSE(SpriteLightMap::sourceOf("composite/-3,-2/2").has_value());
            EXPECT_FALSE(SpriteLightMap::sourceOf("textures/tx_smoke.dds").has_value());
        }
    }
}
