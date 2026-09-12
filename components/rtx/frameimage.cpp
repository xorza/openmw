#include "frameimage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace Rtx
{
    osg::ref_ptr<osg::Image> frameImage(
        const TracedFrame& frame, const int width, const int height, const RowOrder order, const Channels channels)
    {
        if (width <= 0 || height <= 0 || frame.mWidth == 0 || frame.mHeight == 0)
            return nullptr;

        if (frame.mPixels.size() < std::size_t{ frame.mWidth } * frame.mHeight * 4)
            return nullptr;

        const int wide = static_cast<int>(frame.mWidth);
        const int tall = static_cast<int>(frame.mHeight);
        const std::size_t bytes = static_cast<std::size_t>(channels);

        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width, height, 1, channels == Channels::Rgb ? GL_RGB : GL_RGBA, GL_UNSIGNED_BYTE);

        // A row at a time where nothing is being resized or dropped, which is both callers
        // that want the whole frame: at 4K the general path below is eight million short copies.
        if (width == wide && height == tall && channels == Channels::Rgba)
        {
            for (int y = 0; y < height; ++y)
            {
                const int row = order == RowOrder::BottomFirst ? height - 1 - y : y;
                std::memcpy(image->data(0, y), frame.mPixels.data() + static_cast<std::size_t>(row) * wide * 4,
                    static_cast<std::size_t>(wide) * 4);
            }

            return image;
        }

        for (int y = 0; y < height; ++y)
        {
            const int from = order == RowOrder::BottomFirst ? height - 1 - y : y;
            const int row = std::min(tall - 1, from * tall / height);
            std::uint8_t* into = image->data(0, y);

            for (int x = 0; x < width; ++x)
            {
                const int column = std::min(wide - 1, x * wide / width);
                std::memcpy(into + static_cast<std::size_t>(x) * bytes,
                    frame.mPixels.data() + (static_cast<std::size_t>(row) * wide + column) * 4, bytes);
            }
        }

        return image;
    }
}
