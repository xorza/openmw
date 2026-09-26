#pragma once

#include <algorithm>

namespace SceneUtil
{
    /// A rectangle of an image, in texels from its corner: row nought first, as `osg::Image` counts
    /// rows. Named, because the functions that take two of them — a source and a target — would
    /// take a transposed pair without a word.
    struct ImageRegion
    {
        int mX = 0;
        int mY = 0;
        int mWidth = 0;
        int mHeight = 0;

        bool empty() const { return mWidth <= 0 || mHeight <= 0; }

        /// The smallest rectangle holding both. Either empty one is the other.
        ImageRegion joined(const ImageRegion& other) const
        {
            if (empty())
                return other;
            if (other.empty())
                return *this;

            const int left = std::min(mX, other.mX);
            const int bottom = std::min(mY, other.mY);
            const int right = std::max(mX + mWidth, other.mX + other.mWidth);
            const int top = std::max(mY + mHeight, other.mY + other.mHeight);
            return ImageRegion{ left, bottom, right - left, top - bottom };
        }

        bool operator==(const ImageRegion&) const = default;
    };
}
