#include "shadingmap.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "colourblock.hpp"
#include "error.hpp"
#include "texturedata.hpp"

namespace Rtx
{
    namespace
    {
        /// How many times the grid is box blurred.
        ///
        /// Three passes of a box are a close enough Gaussian for anything this coarse, and cost
        /// three adds a cell against an exponential's exp. What matters is that the estimate stays
        /// smooth: a correction with an edge in it would put that edge into the frame.
        constexpr int sBlurPasses = 3;

        /// Rec. 709, in linear light, which is where a luminance means anything.
        float luminanceOf(float red, float green, float blue)
        {
            return 0.2126f * red + 0.7152f * green + 0.0722f * blue;
        }

        float toLinear(float encoded)
        {
            return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

        /// What one block contributes: the sum of its texels' luminance, and how many counted.
        struct BlockLuminance
        {
            float mSum = 0.0f;
            std::uint32_t mCount = 0;
        };

        /// The mean luminance of a block, from its palette and the indices that chose it.
        ///
        /// A transparent texel is not a colour and does not belong in an average of them.
        BlockLuminance blockLuminance(std::span<const std::byte, 8> bytes, bool punchThrough, bool srgb)
        {
            const ColourBlock block = ColourBlock::read(bytes, punchThrough);

            std::array<float, 4> palette{};
            for (std::size_t entry = 0; entry < palette.size(); ++entry)
            {
                const osg::Vec3f& colour = block.mPalette[entry];
                palette[entry] = srgb ? luminanceOf(toLinear(colour.x()), toLinear(colour.y()), toLinear(colour.z()))
                                      : luminanceOf(colour.x(), colour.y(), colour.z());
            }

            BlockLuminance total;
            for (std::size_t texel = 0; texel < 16; ++texel)
            {
                if (block.isTransparent(texel))
                    continue;

                total.mSum += palette[block.indexAt(texel)];
                ++total.mCount;
            }

            return total;
        }

    }

    ShadingMap::ShadingMap()
    {
        mValues.fill(1.0f);
    }

    ShadingMap::ShadingMap(const TextureData& texture)
    {
        assert(!texture.mLevels.empty());

        const MipLevel& level = texture.mLevels.front();
        const std::uint32_t width = std::max(level.mWidth, 1u);
        const std::uint32_t height = std::max(level.mHeight, 1u);
        const bool srgb = isSrgb(texture.mFormat);

        std::array<float, std::size_t{ sExtent } * sExtent> sums{};
        std::array<std::uint32_t, std::size_t{ sExtent } * sExtent> counts{};

        // Where a texel or a block lands in the grid. A texture smaller than the grid leaves cells
        // untouched, which is what the fill below is for.
        const auto cellOf = [&](std::uint32_t x, std::uint32_t y) {
            const std::uint32_t column = std::min(x * sExtent / width, sExtent - 1);
            const std::uint32_t row = std::min(y * sExtent / height, sExtent - 1);
            return std::size_t{ row } * sExtent + column;
        };

        if (const std::uint32_t bytes = blockBytes(texture.mFormat); bytes > 0)
        {
            const std::uint32_t columns = (width + 3) / 4;
            const std::uint32_t rows = (height + 3) / 4;

            // BC2 and BC3 put eight bytes of alpha before the colour block; BC1 is colour alone.
            const std::uint32_t colourAt = bytes - 8;

            for (std::uint32_t row = 0; row < rows; ++row)
                for (std::uint32_t column = 0; column < columns; ++column)
                {
                    const std::size_t at = level.mOffset + (std::size_t{ row } * columns + column) * bytes + colourAt;
                    const BlockLuminance block = blockLuminance(
                        texture.mBytes.subspan(at).first<8>(), texture.mFormat == TextureFormat::Bc1RgbaSrgb, srgb);

                    // The block's own centre decides which cell it falls in, so a block straddling
                    // a boundary is not split between two.
                    const std::size_t cell = cellOf(column * 4 + 2, row * 4 + 2);
                    sums[cell] += block.mSum;
                    counts[cell] += block.mCount;
                }
        }
        else
        {
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const std::size_t at = level.mOffset + (std::size_t{ y } * width + x) * 4;
                    const auto channel = [&](std::size_t offset) {
                        const float value = std::to_integer<std::uint32_t>(texture.mBytes[at + offset]) / 255.0f;
                        return srgb ? toLinear(value) : value;
                    };

                    const std::size_t cell = cellOf(x, y);
                    sums[cell] += luminanceOf(channel(0), channel(1), channel(2));
                    ++counts[cell];
                }
        }

        // A texture smaller than the grid resolves into a handful of cells and leaves the rest
        // empty. Reading those as black would make the estimate a spike and drive the correction
        // into its clamps, so too few texels to resolve shading means the same as having none.
        float total = 0.0f;
        std::uint32_t sampled = 0;
        for (std::size_t cell = 0; cell < mValues.size(); ++cell)
            if (counts[cell] > 0)
            {
                mValues[cell] = sums[cell] / static_cast<float>(counts[cell]);
                total += mValues[cell];
                ++sampled;
            }

        // Nothing at all is a texture of no texels, which the assert above already refused.
        assert(sampled > 0);
        const float average = total / static_cast<float>(sampled);
        for (std::size_t cell = 0; cell < mValues.size(); ++cell)
            if (counts[cell] == 0)
                mValues[cell] = average;

        // Wrapping, because Morrowind's textures tile and a great many of them rely on it: a blur
        // that clamped at the edges would invent a gradient across every wall.
        std::array<float, std::size_t{ sExtent } * sExtent> scratch{};
        for (int pass = 0; pass < sBlurPasses; ++pass)
        {
            for (std::uint32_t y = 0; y < sExtent; ++y)
                for (std::uint32_t x = 0; x < sExtent; ++x)
                {
                    const std::uint32_t left = (x + sExtent - 1) % sExtent;
                    const std::uint32_t right = (x + 1) % sExtent;
                    const std::size_t row = std::size_t{ y } * sExtent;
                    scratch[row + x] = (mValues[row + left] + mValues[row + x] + mValues[row + right]) / 3.0f;
                }

            for (std::uint32_t y = 0; y < sExtent; ++y)
                for (std::uint32_t x = 0; x < sExtent; ++x)
                {
                    const std::size_t above = std::size_t{ (y + sExtent - 1) % sExtent } * sExtent;
                    const std::size_t below = std::size_t{ (y + 1) % sExtent } * sExtent;
                    const std::size_t here = std::size_t{ y } * sExtent;
                    mValues[here + x] = (scratch[above + x] + scratch[here + x] + scratch[below + x]) / 3.0f;
                }
        }

        // **Normalising is what makes this a redistribution rather than a dimmer.** Dividing by a
        // map that averages one moves light from where the texture already had it to where it did
        // not, and leaves the total alone.
        float mean = 0.0f;
        for (const float value : mValues)
            mean += value;

        mean /= static_cast<float>(mValues.size());

        // A texture that is black everywhere has no lighting to redistribute and no scale to
        // divide by, so it keeps the neutral map it would otherwise be given nonsense in place of.
        if (!(mean > 0.0f))
        {
            mValues.fill(1.0f);
            return;
        }

        for (float& value : mValues)
            value = std::clamp(value / mean, sFloor, sCeiling);
    }

    float paintedLight(std::span<const float> map, float u, float v)
    {
        constexpr int extent = static_cast<int>(ShadingMap::sExtent);
        assert(map.size() == std::size_t{ extent } * extent);

        const auto fraction = [](float value) { return value - std::floor(value); };

        const float x = fraction(u) * extent - 0.5f;
        const float y = fraction(v) * extent - 0.5f;
        const auto lowX = static_cast<int>(std::floor(x));
        const auto lowY = static_cast<int>(std::floor(y));
        const float acrossX = x - static_cast<float>(lowX);
        const float acrossY = y - static_cast<float>(lowY);

        // The half-texel back above puts the lowest cell at minus one, which the wrap takes to the
        // far edge — which is the whole point of a tiling map and the one thing a clamp would lose.
        const auto wrap = [](int at) { return (at % extent + extent) % extent; };
        const auto cell = [&](int column, int row) {
            return map[static_cast<std::size_t>(wrap(row)) * extent + static_cast<std::size_t>(wrap(column))];
        };

        const float top = std::lerp(cell(lowX, lowY), cell(lowX + 1, lowY), acrossX);
        const float bottom = std::lerp(cell(lowX, lowY + 1), cell(lowX + 1, lowY + 1), acrossX);

        return std::lerp(top, bottom, acrossY);
    }
}
