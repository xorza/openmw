#include "alphabounds.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "alphaimage.hpp"

namespace Rtx
{
    namespace
    {
        /// One half-open run of texels.
        struct Run
        {
            std::uint32_t mFrom = 0;
            std::uint32_t mTo = 0;
        };

        /// One axis of a box that may sit outside the image, as the runs it becomes inside it.
        ///
        /// Two at most: a box narrower than the image either fits or straddles the seam once, and a
        /// box at least as wide as the image covers all of it however many times it goes round.
        struct WrappedSpan
        {
            Run mRuns[2];
        };

        WrappedSpan wrapSpan(std::int64_t from, std::uint32_t across, std::uint32_t extent)
        {
            if (across >= extent)
                return WrappedSpan{ { Run{ 0, extent }, Run{} } };

            const auto begin = static_cast<std::uint32_t>(((from % extent) + extent) % extent);
            const std::uint32_t end = begin + across;
            if (end <= extent)
                return WrappedSpan{ { Run{ begin, end }, Run{} } };

            return WrappedSpan{ { Run{ begin, extent }, Run{ 0, end - extent } } };
        }

        /// Where a texture coordinate lands on a grid, as a texel that may sit outside it.
        ///
        /// Doubles because a scrolling material's coordinates run away from the unit square without
        /// bound, and clamped because what a box wider than the image covers is the whole image
        /// whatever number stood at its edge.
        std::int64_t floorTexel(float coordinate, std::uint32_t extent)
        {
            constexpr double sLimit = 1e15;

            const double at = std::floor(static_cast<double>(coordinate) * extent);

            return static_cast<std::int64_t>(std::clamp(at, -sLimit, sLimit));
        }
    }

    AlphaBounds::AlphaBounds(const AlphaImage& mask, float cutoff)
    {
        if (mask.isEmpty())
            return;

        mWidth = mask.getWidth();
        mHeight = mask.getHeight();

        // Coarse to fine, because that is the direction the fold runs: a point's reach at a coarse
        // level covers every finer texel under it, so each level's own three-by-three is taken
        // together with what the levels above it already said about the same place.
        std::vector<std::uint8_t> lower;
        std::vector<std::uint8_t> upper;
        std::vector<std::uint8_t> nextLower;
        std::vector<std::uint8_t> nextUpper;
        std::uint32_t coarserWidth = 0;
        std::uint32_t coarserHeight = 0;

        for (std::uint32_t step = mask.getLevelCount(); step-- > 0;)
        {
            const MipLevel& level = mask.getLevel(step);
            const std::size_t count = std::size_t{ level.mWidth } * level.mHeight;

            nextLower.assign(count, 255);
            nextUpper.assign(count, 0);

            for (std::uint32_t y = 0; y < level.mHeight; ++y)
                for (std::uint32_t x = 0; x < level.mWidth; ++x)
                {
                    std::uint8_t low = 255;
                    std::uint8_t high = 0;

                    for (std::uint32_t dy = 0; dy < 3; ++dy)
                        for (std::uint32_t dx = 0; dx < 3; ++dx)
                        {
                            const std::uint32_t nx = (x + level.mWidth + dx - 1) % level.mWidth;
                            const std::uint32_t ny = (y + level.mHeight + dy - 1) % level.mHeight;
                            const std::uint8_t value = mask.at(step, nx, ny);

                            low = std::min(low, value);
                            high = std::max(high, value);
                        }

                    if (coarserWidth != 0)
                    {
                        // Proportional rather than a shift, so a chain that halves unevenly — or
                        // stops halving once an axis reaches one — still lands on the texel above.
                        const std::uint32_t column
                            = static_cast<std::uint32_t>(std::size_t{ x } * coarserWidth / level.mWidth);
                        const std::uint32_t row
                            = static_cast<std::uint32_t>(std::size_t{ y } * coarserHeight / level.mHeight);
                        const std::size_t above = std::size_t{ row } * coarserWidth + column;

                        low = std::min(low, lower[above]);
                        high = std::max(high, upper[above]);
                    }

                    nextLower[std::size_t{ y } * level.mWidth + x] = low;
                    nextUpper[std::size_t{ y } * level.mWidth + x] = high;
                }

            lower.swap(nextLower);
            upper.swap(nextUpper);
            coarserWidth = level.mWidth;
            coarserHeight = level.mHeight;
        }

        // The finest level was the last one folded, so `lower` and `upper` are on its grid.
        const std::size_t stride = std::size_t{ mWidth } + 1;
        mMaterial.assign(stride * (std::size_t{ mHeight } + 1), 0);
        mHole.assign(mMaterial.size(), 0);

        for (std::uint32_t y = 0; y < mHeight; ++y)
        {
            std::uint32_t material = 0;
            std::uint32_t hole = 0;

            for (std::uint32_t x = 0; x < mWidth; ++x)
            {
                const std::size_t texel = std::size_t{ y } * mWidth + x;

                // The same comparison the shader makes, on the same numbers: a byte reaches the
                // sampler as itself over 255, and the cutoff it is tested against is that scale.
                material += static_cast<float>(lower[texel]) * (1.0f / 255.0f) >= cutoff ? 1 : 0;
                hole += static_cast<float>(upper[texel]) * (1.0f / 255.0f) < cutoff ? 1 : 0;

                mMaterial[(std::size_t{ y } + 1) * stride + x + 1]
                    = mMaterial[std::size_t{ y } * stride + x + 1] + material;
                mHole[(std::size_t{ y } + 1) * stride + x + 1] = mHole[std::size_t{ y } * stride + x + 1] + hole;
            }
        }
    }

    std::uint32_t AlphaBounds::countIn(const std::vector<std::uint32_t>& sums, std::uint32_t fromX, std::uint32_t fromY,
        std::uint32_t toX, std::uint32_t toY) const
    {
        const std::size_t stride = std::size_t{ mWidth } + 1;

        return sums[std::size_t{ toY } * stride + toX] - sums[std::size_t{ fromY } * stride + toX]
            - sums[std::size_t{ toY } * stride + fromX] + sums[std::size_t{ fromY } * stride + fromX];
    }

    std::uint64_t AlphaBounds::countWrapped(const std::vector<std::uint32_t>& sums, std::int64_t fromX,
        std::int64_t fromY, std::uint32_t acrossX, std::uint32_t acrossY) const
    {
        const WrappedSpan across = wrapSpan(fromX, acrossX, mWidth);
        const WrappedSpan down = wrapSpan(fromY, acrossY, mHeight);

        std::uint64_t total = 0;
        for (const Run& column : across.mRuns)
            for (const Run& row : down.mRuns)
                if (column.mTo > column.mFrom && row.mTo > row.mFrom)
                    total += countIn(sums, column.mFrom, row.mFrom, column.mTo, row.mTo);

        return total;
    }

    Opacity AlphaBounds::classify(const UvBox& patch) const
    {
        // **Before the ordering is asserted, because a coordinate that is not a number is neither
        // ordered nor unordered.** A patch this cannot locate is one to go on asking about, and a
        // texture coordinate arrives out of a content file rather than out of this code.
        if (!std::isfinite(patch.mFromU) || !std::isfinite(patch.mToU) || !std::isfinite(patch.mFromV)
            || !std::isfinite(patch.mToV))
            return Opacity::Unknown;

        assert(patch.mFromU <= patch.mToU && patch.mFromV <= patch.mToV);

        if (mWidth == 0)
            return Opacity::Unknown;

        const std::int64_t fromX = floorTexel(patch.mFromU, mWidth);
        const std::int64_t fromY = floorTexel(patch.mFromV, mHeight);

        const auto acrossX
            = static_cast<std::uint32_t>(std::min<std::int64_t>(floorTexel(patch.mToU, mWidth) - fromX + 1, mWidth));
        const auto acrossY
            = static_cast<std::uint32_t>(std::min<std::int64_t>(floorTexel(patch.mToV, mHeight) - fromY + 1, mHeight));

        const std::uint64_t texels = std::uint64_t{ acrossX } * acrossY;
        if (countWrapped(mMaterial, fromX, fromY, acrossX, acrossY) == texels)
            return Opacity::Opaque;
        if (countWrapped(mHole, fromX, fromY, acrossX, acrossY) == texels)
            return Opacity::Transparent;

        return Opacity::Unknown;
    }
}
