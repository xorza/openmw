#include "spritelight.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <span>

#include "alphaimage.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::string_view sPrefix = "sprite/";

        /// What a ray keeps for crossing one texel of each alpha a byte can hold, in a level
        /// `count` texels long.
        ///
        /// A table, because the walk below asks it once per texel and `std::pow` would be most of
        /// what the walk costs: 256 entries against a level's texel count, and the same ones for
        /// every row.
        std::array<float, 256> stepsAcross(std::uint32_t count)
        {
            std::array<float, 256> steps{};
            for (std::size_t alpha = 0; alpha < steps.size(); ++alpha)
                steps[alpha] = std::pow(1.0f - static_cast<float>(alpha) / 255.0f, 1.0f / static_cast<float>(count));

            return steps;
        }

        std::uint8_t quantize(float transmittance)
        {
            return static_cast<std::uint8_t>(std::lround(transmittance * 255.0f));
        }
    }

    std::string SpriteLightMap::keyFor(VFS::Path::NormalizedView source)
    {
        std::string key(sPrefix);
        key += source.value();
        return key;
    }

    std::optional<VFS::Path::Normalized> SpriteLightMap::sourceOf(std::string_view key)
    {
        if (!key.starts_with(sPrefix))
            return std::nullopt;

        return VFS::Path::Normalized(key.substr(sPrefix.size()));
    }

    SpriteLightMap::SpriteLightMap(const AlphaImage& alpha)
        : mWidth(alpha.getWidth())
        , mHeight(alpha.getHeight())
    {

        const std::uint32_t count = alpha.getLevelCount();
        mLevels.reserve(count);

        std::size_t bytes = 0;
        for (std::uint32_t level = 0; level < count; ++level)
        {
            const MipLevel& shape = alpha.getLevel(level);
            bytes += std::size_t{ shape.mWidth } * shape.mHeight * 4;
        }
        mBytes.resize(bytes);

        std::uint32_t offset = 0;
        for (std::uint32_t level = 0; level < count; ++level)
        {
            const MipLevel& shape = alpha.getLevel(level);
            const std::uint32_t width = shape.mWidth;
            const std::uint32_t height = shape.mHeight;
            mLevels.push_back(MipLevel{ offset, width, height });

            const std::array<float, 256> alongRow = stepsAcross(width);
            const std::array<float, 256> alongColumn = stepsAcross(height);

            const auto texel = [&](std::uint32_t x, std::uint32_t y) -> std::uint8_t* {
                return mBytes.data() + offset + (std::size_t{ y } * width + x) * 4;
            };

            // Each channel is a running product from the edge the light comes in at, written to a
            // texel *before* that texel's own alpha is multiplied in: what reaches a texel is what
            // the ones beyond it let through, not what it lets through itself.
            for (std::uint32_t y = 0; y < height; ++y)
            {
                float through = 1.0f;
                for (std::uint32_t x = width; x-- > 0;)
                {
                    texel(x, y)[0] = quantize(through);
                    through *= alongRow[alpha.at(level, x, y)];
                }

                through = 1.0f;
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    texel(x, y)[1] = quantize(through);
                    through *= alongRow[alpha.at(level, x, y)];
                }
            }

            for (std::uint32_t x = 0; x < width; ++x)
            {
                float through = 1.0f;
                for (std::uint32_t y = height; y-- > 0;)
                {
                    texel(x, y)[2] = quantize(through);
                    through *= alongColumn[alpha.at(level, x, y)];
                }

                through = 1.0f;
                for (std::uint32_t y = 0; y < height; ++y)
                {
                    texel(x, y)[3] = quantize(through);
                    through *= alongColumn[alpha.at(level, x, y)];
                }
            }

            offset += width * height * 4;
        }
    }

    TextureData SpriteLightMap::describe() const
    {
        return TextureData{
            .mFormat = TextureFormat::Rgba8Unorm,
            .mWidth = mWidth,
            .mHeight = mHeight,
            .mBytes = std::as_bytes(std::span(mBytes)),
            .mLevels = mLevels,
            .mName = "sprite light",
        };
    }

    std::uint8_t SpriteLightMap::at(std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t channel) const
    {
        const MipLevel& shape = mLevels[level];
        assert(x < shape.mWidth && y < shape.mHeight && channel < 4);

        return mBytes[shape.mOffset + (std::size_t{ y } * shape.mWidth + x) * 4 + channel];
    }
}
