#include "formatcensus.hpp"

#include <cassert>
#include <cstddef>

#include <osg/Image>

#include "texels.hpp"

namespace Rtx
{
    void FormatCensus::count(const osg::Image& image, const TextureEncoding encoding)
    {
        const TextureFormat format = readFormat(image, encoding);

        FormatCount& met = mMet[static_cast<std::size_t>(format)];
        ++met.mMet;
        if (image.getNumMipmapLevels() > 1)
            ++met.mMipped;

        if (format == TextureFormat::Unnamed)
            mUnnamed = static_cast<std::uint32_t>(image.getPixelFormat());
    }

    void FormatCensus::discount(const osg::Image& image, const TextureEncoding encoding)
    {
        FormatCount& met = mMet[static_cast<std::size_t>(readFormat(image, encoding))];
        assert(met.mMet > 0 && "a texture discounted that was never counted");
        --met.mMet;
        if (image.getNumMipmapLevels() > 1)
            --met.mMipped;
    }
}
