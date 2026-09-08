#include "alphaimage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include <osg/Image>

#include <components/debug/debuglog.hpp>

#include "colourblock.hpp"
#include "error.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// Where a texel sits inside its four-by-four block, counting along rows from the top left —
        /// which is the order every one of these formats indexes by.
        std::size_t texelInBlock(std::uint32_t x, std::uint32_t y)
        {
            return (y % 4) * 4 + (x % 4);
        }

        /// BC2's alpha: four bits a texel, sixteen of them in the block's first eight bytes.
        ///
        /// Widened by seventeen rather than by shifting four places, so that fifteen lands on 255
        /// and not on 240 — the difference is whether a fully opaque texel reads as fully opaque.
        std::uint8_t bc2Alpha(std::span<const std::byte, 8> bytes, std::size_t texel)
        {
            const auto packed = static_cast<std::uint8_t>(bytes[texel / 2]);
            const std::uint8_t nibble = texel % 2 == 0 ? packed & 0x0Fu : packed >> 4;

            return static_cast<std::uint8_t>(nibble * 17);
        }

        /// BC3's alpha: two endpoints and sixteen three-bit indices into a palette built from them.
        ///
        /// **Which palette depends on the order the endpoints are stored in**, exactly as BC1's
        /// colour does: descending gives eight interpolated values, ascending gives six and spends
        /// the last two entries on nought and full. A decoder that assumed one of them reads every
        /// texel of half the blocks wrong.
        std::uint8_t bc3Alpha(std::span<const std::byte, 8> bytes, std::size_t texel)
        {
            const auto first = static_cast<std::uint8_t>(bytes[0]);
            const auto second = static_cast<std::uint8_t>(bytes[1]);

            std::array<std::uint8_t, 8> palette{ first, second };
            if (first > second)
                for (std::size_t i = 1; i < 7; ++i)
                    palette[i + 1] = static_cast<std::uint8_t>(((7 - i) * first + i * second) / 7);
            else
            {
                for (std::size_t i = 1; i < 5; ++i)
                    palette[i + 1] = static_cast<std::uint8_t>(((5 - i) * first + i * second) / 5);

                palette[6] = 0;
                palette[7] = 255;
            }

            // Forty-eight bits, little-endian across the six bytes that follow the endpoints.
            std::uint64_t indices = 0;
            for (std::size_t i = 0; i < 6; ++i)
                indices |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[2 + i])) << (i * 8);

            return palette[(indices >> (texel * 3)) & 0x7u];
        }

        /// One level's alpha, decoded into `into` in row order.
        ///
        /// **Every block format is read through the same walk**, because they differ only in how
        /// many bytes a block is and where its alpha sits inside one. A level whose bytes run short
        /// keeps the fully opaque values it was filled with, which is the same answer a texture
        /// that could not be read gets and for the same reason.
        void decodeLevel(TextureFormat format, std::span<const std::byte> bytes, std::uint32_t width,
            std::uint32_t height, std::span<std::uint8_t> into)
        {
            const std::uint32_t bytesPerBlock = blockBytes(format);
            const std::uint32_t blocksAcross = (width + 3) / 4;

            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    std::uint8_t& value = into[std::size_t{ y } * width + x];

                    if (bytesPerBlock == 0)
                    {
                        // Four bytes a texel in every uncompressed spelling, and alpha is the last
                        // of them whichever order the three colours are stated in.
                        const std::size_t at = (std::size_t{ y } * width + x) * 4 + 3;
                        if (at < bytes.size())
                            value = static_cast<std::uint8_t>(bytes[at]);

                        continue;
                    }

                    const std::size_t block = (std::size_t{ y / 4 } * blocksAcross + x / 4) * bytesPerBlock;
                    if (block + bytesPerBlock > bytes.size())
                        continue;

                    const std::size_t texel = texelInBlock(x, y);
                    switch (format)
                    {
                        case TextureFormat::Bc1RgbaSrgb:
                        {
                            // BC1 has no alpha channel — it has a fourth palette entry that means
                            // "nothing here", and only when the endpoints are stored ascending.
                            const ColourBlock read = ColourBlock::read(bytes.subspan(block).first<8>(), true);
                            value = read.isTransparent(texel) ? 0 : 255;
                            break;
                        }
                        case TextureFormat::Bc2Srgb:
                            value = bc2Alpha(bytes.subspan(block).first<8>(), texel);
                            break;
                        case TextureFormat::Bc3Srgb:
                            value = bc3Alpha(bytes.subspan(block).first<8>(), texel);
                            break;
                        default:
                            break;
                    }
                }
        }
    }

    void AlphaImage::build(const TextureData& texture)
    {
        mValues.clear();

        const std::size_t texels = mShape.layOutLike(texture.mLevels, 1);
        if (texels == 0)
            return;

        mValues.assign(texels, std::uint8_t{ 255 });

        for (std::uint32_t at = 0; at < mShape.getLevelCount(); ++at)
        {
            const MipLevel& into = mShape.getLevel(at);
            const std::size_t count = std::size_t{ into.mWidth } * into.mHeight;
            if (count == 0)
                continue;

            // Clamped rather than trusted: a level whose offset runs past the bytes decodes nothing
            // and keeps the fully opaque values it was filled with, which is the answer a texture
            // that could not be read gets already.
            const std::size_t from = std::min<std::size_t>(texture.mLevels[at].mOffset, texture.mBytes.size());
            decodeLevel(texture.mFormat, texture.mBytes.subspan(from), into.mWidth, into.mHeight,
                std::span(mValues).subspan(into.mOffset, count));
        }
    }

    bool reachesSolid(const osg::Image& image, AlphaScratch& scratch)
    {
        std::vector<MipLevel>& levels = scratch.mLevels;
        levels.clear();

        TextureData described;
        try
        {
            described = describeImage(image, levels);
        }
        catch (const Error& what)
        {
            // A format nothing in the game produces, which is a mod's business rather than a broken
            // contract. The caller gets the answer that changes nothing about how the surface is
            // traced.
            Log(Debug::Warning) << "cannot read the alpha of \"" << image.getFileName() << "\": " << what.what();
            return true;
        }

        if (levels.empty() || levels.front().mWidth == 0 || levels.front().mHeight == 0)
            return true;

        // **Decoded down to the one level that can answer**, which is three quarters of the work a
        // whole chain would be: every coarser level is an average of the one above it, and a mask's
        // average stops reaching solid a level or two down. Handing the description one level is
        // what says that in code rather than in a comment over a loop that reads `at(0, ...)`.
        described.mLevels = described.mLevels.subspan(0, 1);

        scratch.mAlpha.build(described);
        const MipLevel& level = levels.front();

        for (std::uint32_t y = 0; y < level.mHeight; ++y)
            for (std::uint32_t x = 0; x < level.mWidth; ++x)
                if (scratch.mAlpha.at(0, x, y) == 255)
                    return true;

        return false;
    }
}
