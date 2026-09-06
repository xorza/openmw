#include "mipchain.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <osg/Vec3f>

#include "srgb.hpp"
#include "texelreader.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t halved(std::uint32_t side)
        {
            return std::max(side / 2, 1u);
        }

        std::byte quantise(float value)
        {
            return static_cast<std::byte>(std::clamp(std::lround(value * 255.0f), 0L, 255L));
        }
    }

    void MipChain::build(const TextureData& described)
    {
        mLevels.clear();
        mTexels.clear();
        mWidth = 0;
        mHeight = 0;
        mEncoded = true;
        mName = {};

        if (described.mLevels.empty())
            return;

        // **Only a file that carried no chain at all.** Morrowind's own stop short of a single
        // texel — a 256-square texture ships six levels and ends at 8 by 8 — and that last level is
        // already the texture's own mean to within what a ray can tell. Rebuilding those would
        // decompress the whole game to gain nothing: measured, it put the resident textures of one
        // cell from 75 MB to 149 MB.
        if (described.mLevels.size() != 1)
            return;

        // A level with no extent is a level with no texels to read, and every reader below would
        // be asked for one.
        const MipLevel& finest = described.mLevels.front();
        if (finest.mWidth == 0 || finest.mHeight == 0 || (finest.mWidth == 1 && finest.mHeight == 1))
            return;

        mWidth = finest.mWidth;
        mHeight = finest.mHeight;
        mEncoded = isSrgb(described.mFormat);
        mName = described.mName;

        // The whole shape first, so the texels are asked for once and the levels never move.
        std::size_t bytes = 0;
        for (MipLevel level{ .mOffset = 0, .mWidth = mWidth, .mHeight = mHeight };;)
        {
            mLevels.push_back(level);
            bytes += std::size_t{ level.mWidth } * level.mHeight * 4;

            if (level.mWidth == 1 && level.mHeight == 1)
                break;

            level = MipLevel{
                .mOffset = static_cast<std::uint32_t>(bytes),
                .mWidth = halved(level.mWidth),
                .mHeight = halved(level.mHeight),
            };
        }

        mTexels.resize(bytes);

        // **The finest level, through the readers that already know every format.** Alpha is a byte
        // a texel in all of them and colour is one call apiece, so nothing here knows what a block
        // is.
        mAlpha.build(described);
        for (std::uint32_t y = 0; y < mHeight; ++y)
            for (std::uint32_t x = 0; x < mWidth; ++x)
            {
                const osg::Vec3f colour = texelAt(described, finest, x, y);
                const std::size_t at = (std::size_t{ y } * mWidth + x) * 4;

                for (int channel = 0; channel < 3; ++channel)
                    mTexels[at + static_cast<std::size_t>(channel)] = quantise(colour[channel]);

                mTexels[at + 3] = static_cast<std::byte>(mAlpha.at(0, x, y));
            }

        // **Each level from the one above it, with the colours weighed by the alpha they carry.** A
        // texel nothing painted has no colour to average in — a punch-through block stores black
        // there — so an even mean draws a dark rim round every leaf and every drop as the chain goes
        // down. Where a whole quad of them is empty there is nothing to weigh, and the even mean is
        // the only answer left.
        //
        // In light and not in bytes, for the reason `Rtx::toLinear` gives: the mean of two stored
        // bytes is not the byte of their mean.
        for (std::size_t at = 1; at < mLevels.size(); ++at)
        {
            const MipLevel& above = mLevels[at - 1];
            const MipLevel& level = mLevels[at];

            for (std::uint32_t y = 0; y < level.mHeight; ++y)
                for (std::uint32_t x = 0; x < level.mWidth; ++x)
                {
                    osg::Vec3f weighed;
                    osg::Vec3f even;
                    float painted = 0.0f;

                    for (const std::uint32_t dy : { 0u, 1u })
                        for (const std::uint32_t dx : { 0u, 1u })
                        {
                            const std::uint32_t sx = std::min(2 * x + dx, above.mWidth - 1);
                            const std::uint32_t sy = std::min(2 * y + dy, above.mHeight - 1);
                            const std::size_t from = above.mOffset + (std::size_t{ sy } * above.mWidth + sx) * 4;

                            const auto stored = [&](std::size_t offset) {
                                return std::to_integer<std::uint8_t>(mTexels[from + offset]);
                            };

                            // **Through the byte and not through a float divided by 255**, which is
                            // the same number by a table rather than by a `pow` a texel a channel a
                            // level. Alpha is linear in every format and is the byte's own share.
                            const auto channel = [&](std::size_t offset) {
                                return mEncoded ? toLinear(stored(offset)) : stored(offset) / 255.0f;
                            };

                            const osg::Vec3f texel(channel(0), channel(1), channel(2));
                            const float alphaHere = stored(3) / 255.0f;
                            even += texel;
                            weighed += texel * alphaHere;
                            painted += alphaHere;
                        }

                    const osg::Vec3f mean = painted > 0.0f ? weighed / painted : even / 4.0f;

                    const std::size_t into = level.mOffset + (std::size_t{ y } * level.mWidth + x) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        mTexels[into + static_cast<std::size_t>(channel)]
                            = quantise(mEncoded ? toEncoded(mean[channel]) : mean[channel]);

                    mTexels[into + 3] = quantise(painted / 4.0f);
                }
        }
    }

    TextureData MipChain::describe() const
    {
        return TextureData{
            .mFormat = mEncoded ? TextureFormat::Rgba8Srgb : TextureFormat::Rgba8Unorm,
            .mWidth = mWidth,
            .mHeight = mHeight,
            .mBytes = mTexels,
            .mLevels = mLevels,
            .mName = mName,
        };
    }
}
