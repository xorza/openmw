#include "texturedata.hpp"

#include <algorithm>
#include <bit>

namespace Rtx
{
    std::size_t MipPyramid::layOutTo1x1(const std::uint32_t width, const std::uint32_t height, const std::size_t stride)
    {
        mLevels.clear();

        if (width == 0 || height == 0)
            return 0;

        // Exactly, because a bake that grew this vector as it walked reached the heap once a level
        // — which is what `aScratchTheCallerKeepsLeavesABakeNothingButItsAnswerToAllocate` counts.
        mLevels.reserve(std::bit_width(std::max(width, height)));

        std::size_t bytes = 0;
        for (MipLevel level{ .mOffset = 0, .mWidth = width, .mHeight = height };;)
        {
            mLevels.push_back(level);
            bytes += std::size_t{ level.mWidth } * level.mHeight * stride;

            if (level.mWidth == 1 && level.mHeight == 1)
                break;

            level = MipLevel{
                .mOffset = static_cast<std::uint32_t>(bytes),
                .mWidth = std::max(level.mWidth / 2, 1u),
                .mHeight = std::max(level.mHeight / 2, 1u),
            };
        }

        return bytes;
    }

    std::size_t MipPyramid::layOutLike(const std::span<const MipLevel> shape, const std::size_t stride)
    {
        mLevels.clear();
        mLevels.reserve(shape.size());

        std::size_t bytes = 0;
        for (const MipLevel& level : shape)
        {
            mLevels.push_back(MipLevel{
                .mOffset = static_cast<std::uint32_t>(bytes),
                .mWidth = level.mWidth,
                .mHeight = level.mHeight,
            });

            bytes += std::size_t{ level.mWidth } * level.mHeight * stride;
        }

        return bytes;
    }
}
