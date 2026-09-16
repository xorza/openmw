#pragma once

#include <cstdint>

namespace osg
{
    class Image;
}

namespace MyGUIRtx
{
    /// `count` rows of `image` from `firstRow`, into `into` as four bytes a pixel. One `memcpy`
    /// where the image already is that, and a pixel at a time where it is not:
    /// `osg::Image::getColor` is the only thing that reads every format OpenSceneGraph loads, and
    /// it is a virtual call and a `Vec4f` per pixel.
    void writeRgbaRows(const osg::Image& image, int firstRow, int count, std::uint8_t* into);
}
