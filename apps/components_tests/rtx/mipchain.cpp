#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include <gtest/gtest.h>

#include <components/rtx/mipchain.hpp>
#include <components/rtx/texturedata.hpp>

#include "testtexture.hpp"

namespace Rtx
{
    namespace
    {
        using Testing::TestTexture;

        /// Adds one level, painted from `bytes` or left blank where none are given.
        ///
        /// A blank one is for a test that only wants the file to claim the level: nothing reads its
        /// texels, because a chain that is already there is never filtered.
        void addLevel(TestTexture& texture, std::uint32_t width, std::uint32_t height,
            std::initializer_list<std::uint8_t> bytes = {})
        {
            texture.mLevels.push_back(MipLevel{ static_cast<std::uint32_t>(texture.mBytes.size()), width, height });

            if (bytes.size() > 0)
                texture.mBytes.insert(texture.mBytes.end(), bytes);
            else
                texture.mBytes.resize(texture.mBytes.size() + std::size_t{ width } * height * 4);
        }

        /// One channel of one texel of a built level.
        std::uint32_t channelAt(
            const TextureData& texture, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::size_t channel)
        {
            const MipLevel& which = texture.mLevels[level];

            return std::to_integer<std::uint32_t>(
                texture.mBytes[which.mOffset + (std::size_t{ y } * which.mWidth + x) * 4 + channel]);
        }

        /// A texture that carried its own chain is left exactly as it arrived.
        ///
        /// **The ordinary case, and it has to cost nothing.** Five thousand of Morrowind's textures
        /// ship a chain; a builder that rebuilt them would decode every block in the game on every
        /// cell load, and would upload them loose.
        TEST(RtxMipChainTest, aTextureThatCarriedItsOwnChainIsLeftAlone)
        {
            TestTexture whole;
            addLevel(whole, 4, 4);
            addLevel(whole, 2, 2);
            addLevel(whole, 1, 1);
            whole.describe(4, 4, "whole");

            EXPECT_TRUE(MipChain(whole.mData).isEmpty());

            // **And a chain that stops short is a chain.** Morrowind's own end at eight texels
            // rather than at one, and rebuilding those would decompress the whole game to gain a
            // level no ray can tell from the one above it.
            TestTexture partial;
            addLevel(partial, 4, 4);
            addLevel(partial, 2, 2);
            partial.describe(4, 4, "partial");

            EXPECT_TRUE(MipChain(partial.mData).isEmpty());

            // **And a single texel is a whole chain**, which is the one extent that needs no levels
            // under it.
            TestTexture one;
            addLevel(one, 1, 1, { 0, 0, 0, 255 });
            one.describe(1, 1, "one");

            EXPECT_TRUE(MipChain(one.mData).isEmpty());
        }

        /// Every level a file left out is the mean of the one above it.
        ///
        /// Four quads of one value apiece — 100, 200, 0 and 40 — so the second level is those four
        /// numbers and the third is their mean, `(100 + 200 + 0 + 40) / 4 = 85`. Opaque throughout,
        /// which is what makes the weighing below a separate question from this one.
        TEST(RtxMipChainTest, everyLevelAFileLeftOutIsTheMeanOfTheOneAboveIt)
        {
            TestTexture four;
            for (const std::uint8_t value : { 100, 200, 100, 200, 0, 40, 0, 40 })
                for (int twice = 0; twice < 2; ++twice)
                    for (const std::uint8_t byte : { value, value, value, std::uint8_t{ 255 } })
                        four.mBytes.push_back(byte);

            four.mLevels.push_back(MipLevel{ 0, 4, 4 });
            four.describe(4, 4, "four");

            const MipChain chain(four.mData);
            ASSERT_FALSE(chain.isEmpty());

            const TextureData built = chain.describe();
            ASSERT_EQ(built.mLevels.size(), 3u) << "4 by 4 runs down to one texel in three levels";
            EXPECT_EQ(built.mWidth, 4u);
            EXPECT_EQ(built.mHeight, 4u);
            EXPECT_EQ(built.mFormat, TextureFormat::Rgba8Unorm) << "what has no curve under it keeps none";

            // The finest level is the file's own, carried across rather than filtered.
            EXPECT_EQ(channelAt(built, 0, 0, 0, 0), 100u);
            EXPECT_EQ(channelAt(built, 0, 3, 3, 0), 40u);

            EXPECT_EQ(built.mLevels[1].mWidth, 2u);
            EXPECT_EQ(channelAt(built, 1, 0, 0, 0), 100u);
            EXPECT_EQ(channelAt(built, 1, 1, 0, 0), 200u);
            EXPECT_EQ(channelAt(built, 1, 0, 1, 0), 0u);
            EXPECT_EQ(channelAt(built, 1, 1, 1, 0), 40u);

            EXPECT_EQ(built.mLevels[2].mWidth, 1u);
            EXPECT_EQ(channelAt(built, 2, 0, 0, 0), 85u) << "the mean of the four quads";
            EXPECT_EQ(channelAt(built, 2, 0, 0, 3), 255u) << "nothing was transparent, so nothing faded";
        }

        /// A colour is weighed by the alpha carrying it, and the alpha is not.
        ///
        /// **A texel nothing painted has no colour**, and a punch-through block stores black where
        /// it painted nothing — so an even mean draws a dark rim round every leaf card and every
        /// raindrop one level down. Two white texels at full alpha beside two black ones at none
        /// come to white at half alpha, where an even mean would come to 127.
        TEST(RtxMipChainTest, aColourIsWeighedByTheAlphaCarryingItAndTheAlphaIsNot)
        {
            TestTexture pair;
            addLevel(pair, 2, 2, { 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0 });
            pair.describe(2, 2, "pair");

            const MipChain chain(pair.mData);
            ASSERT_FALSE(chain.isEmpty());

            const TextureData built = chain.describe();
            ASSERT_EQ(built.mLevels.size(), 2u);

            EXPECT_EQ(channelAt(built, 1, 0, 0, 0), 255u) << "the black the transparent texels stored was averaged in";
            EXPECT_EQ(channelAt(built, 1, 0, 0, 3), 128u) << "half of it was painted";

            // **And a quad with nothing painted in it still has a colour to state.** There is no
            // weight to divide by there, so the even mean is what is left.
            TestTexture empty;
            addLevel(empty, 2, 2, { 60, 60, 60, 0, 20, 20, 20, 0, 60, 60, 60, 0, 20, 20, 20, 0 });
            empty.describe(2, 2, "empty");

            const MipChain none(empty.mData);
            ASSERT_FALSE(none.isEmpty());
            EXPECT_EQ(channelAt(none.describe(), 1, 0, 0, 0), 40u);
            EXPECT_EQ(channelAt(none.describe(), 1, 0, 0, 3), 0u);
        }

        /// A display-encoded texture is averaged in light and written back encoded.
        ///
        /// **The number `Rtx::toLinear` names.** Half of nothing and half of white meet at 188 in
        /// light and at 128 in bytes, and the second is every fade in the game coming out muddy.
        /// Which of the two this does is the whole difference between an sRGB format and the one
        /// above.
        TEST(RtxMipChainTest, aDisplayEncodedTextureIsAveragedInLight)
        {
            TestTexture pair;
            addLevel(pair, 2, 2, { 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255 });
            pair.describe(2, 2, "pair", TextureFormat::Rgba8Srgb);

            const MipChain chain(pair.mData);
            ASSERT_FALSE(chain.isEmpty());

            const TextureData built = chain.describe();
            EXPECT_EQ(built.mFormat, TextureFormat::Rgba8Srgb) << "what arrived encoded stays encoded";
            EXPECT_EQ(channelAt(built, 1, 0, 0, 0), 188u);
        }
    }
}
