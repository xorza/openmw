#include "mipchain.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <osg/Vec3f>

#include "colour.hpp"
#include "texels.hpp"

namespace Rtx
{
    namespace
    {
        std::byte quantise(float value)
        {
            return static_cast<std::byte>(std::clamp(std::lround(value * 255.0f), 0L, 255L));
        }
    }

    bool MipChain::wantedFor(const TextureData& described)
    {
        return described.mLevels.size() == 1 && std::size_t{ described.mWidth } * described.mHeight > 1;
    }

    void MipChain::build(const TextureData& described)
    {
        mTexture.reuse();
        mEncoded = true;

        if (!wantedFor(described))
            return;

        const MipLevel& finest = described.mLevels.front();
        mEncoded = isSrgb(described.mFormat);

        // The whole shape first, so the texels are asked for once and the levels never move.
        mTexture.openChain(
            finest.mWidth, finest.mHeight, mEncoded ? TextureFormat::Rgba8Srgb : TextureFormat::Rgba8Unorm);
        mTexture.setName(described.mName);

        const std::uint32_t width = mTexture.getWidth();
        const std::uint32_t height = mTexture.getHeight();

        // The finest level, through the readers that already know every format. Alpha is a byte
        // a texel in all of them and colour is one call apiece, so nothing here knows what a block
        // is.
        mAlpha.build(described);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const osg::Vec3f colour = texelAt(described, finest, x, y);
                const std::span<std::byte, OwnedTexture::sStride> into = mTexture.at(0, x, y);

                for (int channel = 0; channel < 3; ++channel)
                    into[static_cast<std::size_t>(channel)] = quantise(colour[channel]);

                into[3] = static_cast<std::byte>(mAlpha.at(0, x, y));
            }

        // Each level from the one above it, with the colours weighed by the alpha they carry,
        // because a punch-through block stores black where nothing was painted and an even mean
        // draws a dark rim round every leaf. In light and not in bytes, because the mean of two
        // stored bytes is not the byte of their mean.
        for (std::uint32_t at = 1; at < mTexture.getShape().getLevelCount(); ++at)
        {
            const MipLevel above = mTexture.getShape().getLevel(at - 1);
            const MipLevel level = mTexture.getShape().getLevel(at);

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
                            const std::span<const std::byte, OwnedTexture::sStride> from
                                = std::as_const(mTexture).at(at - 1, sx, sy);

                            const auto stored
                                = [&](std::size_t offset) { return std::to_integer<std::uint8_t>(from[offset]); };

                            // Through the byte and not through a float divided by 255, which is
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

                    const std::span<std::byte, OwnedTexture::sStride> into = mTexture.at(at, x, y);
                    for (int channel = 0; channel < 3; ++channel)
                        into[static_cast<std::size_t>(channel)]
                            = quantise(mEncoded ? toEncoded(mean[channel]) : mean[channel]);

                    into[3] = quantise(painted / 4.0f);
                }
        }
    }

    TextureData MipChain::describe() const
    {
        return mTexture.describe();
    }
}
