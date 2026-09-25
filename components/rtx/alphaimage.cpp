#include "alphaimage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <osg/Image>

#include "colour.hpp"
#include "result.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// The sixteen alphas of one block, counting along rows from the top left, which is the
        /// order every one of these formats indexes by.
        using BlockAlpha = std::array<std::uint8_t, 16>;

        /// BC2's alpha: four bits a texel, sixteen of them in the block's first eight bytes.
        /// Widened by seventeen rather than by shifting four places, so that fifteen lands on 255
        /// and not on 240 — the difference is whether a fully opaque texel reads as fully opaque.
        void bc2Alpha(std::span<const std::byte, 8> bytes, BlockAlpha& into)
        {
            for (std::size_t texel = 0; texel < into.size(); ++texel)
            {
                const auto packed = static_cast<std::uint8_t>(bytes[texel / 2]);
                const std::uint8_t nibble = texel % 2 == 0 ? packed & 0x0Fu : packed >> 4;
                into[texel] = static_cast<std::uint8_t>(nibble * 17);
            }
        }

        /// BC3's alpha: two endpoints and sixteen three-bit indices into a palette built from them.
        /// Which palette depends on the order the endpoints are stored in, exactly as BC1's
        /// colour does: descending gives eight interpolated values, ascending gives six and spends
        /// the last two entries on nought and full. A decoder that assumed one of them reads every
        /// texel of half the blocks wrong.
        void bc3Alpha(std::span<const std::byte, 8> bytes, BlockAlpha& into)
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

            for (std::size_t texel = 0; texel < into.size(); ++texel)
                into[texel] = palette[(indices >> (texel * 3)) & 0x7u];
        }

        /// One block's alpha, whichever format it is in. The palette is built once for the block
        /// and not once for each of its sixteen texels, which is what a decode of every level of
        /// every sprite pays for otherwise.
        void decodeBlock(TextureFormat format, std::span<const std::byte, 8> bytes, BlockAlpha& into)
        {
            switch (format)
            {
                case TextureFormat::Bc1RgbaSrgb:
                case TextureFormat::Bc1RgbaUnorm:
                {
                    // BC1 has no alpha channel — it has a fourth palette entry that means
                    // "nothing here", and only when the endpoints are stored ascending.
                    const ColourBlock read = ColourBlock::read(bytes, true);
                    for (std::size_t texel = 0; texel < into.size(); ++texel)
                        into[texel] = read.isTransparent(texel) ? 0 : 255;
                    break;
                }
                case TextureFormat::Bc2Srgb:
                case TextureFormat::Bc2Unorm:
                    bc2Alpha(bytes, into);
                    break;
                case TextureFormat::Bc3Srgb:
                case TextureFormat::Bc3Unorm:
                    bc3Alpha(bytes, into);
                    break;
                default:
                    into.fill(255);
                    break;
            }
        }

        /// Hands `visit` every texel of one level with its alpha, in row order — a block at a time
        /// for a block format, so a palette is built once a block — until `visit` answers true.
        /// A level whose bytes run short hands nothing for the texels past them, which leaves a
        /// caller with whatever it started from: the fully opaque values `build` fills with, or the
        /// "reaches nothing" a scan started from — the answer a texture that could not be read
        /// gets, for the same reason. Answers whether `visit` stopped it.
        template <class Visit>
        bool forEachAlpha(TextureFormat format, std::span<const std::byte> bytes, std::uint32_t width,
            std::uint32_t height, Visit visit)
        {
            const TexelLayout layout = layoutOf(format);
            const std::uint32_t bytesPerBlock = layout.mBytes;

            if (!layout.isBlocked())
            {
                // Alpha is the last byte of a loose texel whichever order the three colours are
                // stated in, and every loose format a description carries is four bytes a texel.
                assert(bytesPerBlock == 4 && "a loose texel read as four bytes that is not");
                for (std::uint32_t y = 0; y < height; ++y)
                    for (std::uint32_t x = 0; x < width; ++x)
                    {
                        const std::size_t at = (std::size_t{ y } * width + x) * bytesPerBlock + 3;
                        if (at < bytes.size() && visit(x, y, static_cast<std::uint8_t>(bytes[at])))
                            return true;
                    }

                return false;
            }

            const std::uint32_t blocksAcross = (width + 3) / 4;
            const std::uint32_t blocksDown = (height + 3) / 4;
            BlockAlpha alphas;

            for (std::uint32_t row = 0; row < blocksDown; ++row)
                for (std::uint32_t column = 0; column < blocksAcross; ++column)
                {
                    const std::size_t block = (std::size_t{ row } * blocksAcross + column) * bytesPerBlock;
                    if (block + bytesPerBlock > bytes.size())
                        continue;

                    // BC2 and BC3 put their eight bytes of alpha first; BC1's block is its colour.
                    decodeBlock(format, bytes.subspan(block).first<8>(), alphas);

                    // The texels inside the image alone: a block past its edge pads with texels
                    // nothing draws, and one of those must not answer for the texture.
                    const std::uint32_t rows = std::min(4u, height - row * 4);
                    const std::uint32_t columns = std::min(4u, width - column * 4);
                    for (std::uint32_t dy = 0; dy < rows; ++dy)
                        for (std::uint32_t dx = 0; dx < columns; ++dx)
                            if (visit(column * 4 + dx, row * 4 + dy, alphas[dy * 4 + dx]))
                                return true;
                }

            return false;
        }

        /// One level's alpha, decoded into `into` in row order.
        void decodeLevel(TextureFormat format, std::span<const std::byte> bytes, std::uint32_t width,
            std::uint32_t height, std::span<std::uint8_t> into)
        {
            forEachAlpha(format, bytes, width, height, [&](std::uint32_t x, std::uint32_t y, std::uint8_t alpha) {
                into[std::size_t{ y } * width + x] = alpha;
                return false;
            });
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
        scratch.mTexels.clear();

        // An image this cannot describe is the same image whose arrival in the texture table
        // refuses it by name. What comes back here is the answer that changes nothing about how
        // the surface is traced.
        const Result<TextureData, std::string> read = describeImage(image, levels, scratch.mTexels);
        if (!read.isOk())
            return true;

        const TextureData& described = read.value();

        // The one level that can answer, and only as far as the first solid texel: every coarser
        // level is an average of the one above it, and a mask's average stops reaching solid a
        // level or two down; and nearly every map that reaches solid does so in its first block,
        // so the walk that decodes the level whole is paid by the clouds alone, which never do.
        const MipLevel& level = levels.front();
        const std::size_t from = std::min<std::size_t>(level.mOffset, described.mBytes.size());

        return forEachAlpha(described.mFormat, described.mBytes.subspan(from), level.mWidth, level.mHeight,
            [](std::uint32_t, std::uint32_t, std::uint8_t alpha) { return alpha == 255; });
    }
}
