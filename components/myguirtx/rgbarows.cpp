#include "rgbarows.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

#include <osg/GL>
#include <osg/Image>
#include <osg/Vec4f>

namespace MyGUIRtx
{
    void writeRgbaRows(const osg::Image& image, const int firstRow, const int count, std::uint8_t* into)
    {
        assert(firstRow >= 0 && count >= 0 && firstRow + count <= image.t());
        const std::size_t pixels = static_cast<std::size_t>(image.s()) * count;
        if (image.getPixelFormat() == GL_RGBA && image.getDataType() == GL_UNSIGNED_BYTE && image.isDataContiguous()
            && image.getTotalSizeInBytes() == static_cast<std::size_t>(image.s()) * image.t() * 4)
        {
            std::memcpy(into, image.data(0, firstRow), pixels * 4);
            return;
        }

        for (int y = firstRow; y < firstRow + count; ++y)
            for (int x = 0; x < image.s(); ++x, into += 4)
            {
                const osg::Vec4f colour = image.getColor(x, y);
                into[0] = static_cast<std::uint8_t>(std::clamp(colour.r(), 0.f, 1.f) * 255.f + 0.5f);
                into[1] = static_cast<std::uint8_t>(std::clamp(colour.g(), 0.f, 1.f) * 255.f + 0.5f);
                into[2] = static_cast<std::uint8_t>(std::clamp(colour.b(), 0.f, 1.f) * 255.f + 0.5f);
                into[3] = static_cast<std::uint8_t>(std::clamp(colour.a(), 0.f, 1.f) * 255.f + 0.5f);
            }
    }
}
