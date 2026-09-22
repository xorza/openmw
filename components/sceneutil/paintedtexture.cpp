#include "paintedtexture.hpp"

#include <cassert>

#include <osg/GL>

namespace SceneUtil
{
    PaintedTexture::PaintedTexture(osg::Image* image)
    {
        assert(image != nullptr && image->getPixelFormat() == GL_RGBA && image->getDataType() == GL_UNSIGNED_BYTE
            && image->isDataContiguous() && "a painted picture is tightly packed RGBA8");

        setImage(image);
        setUnRefImageDataAfterApply(false);

        setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    }

    void PaintedTexture::paint(const ImageRegion& region)
    {
        if (region.empty())
            return;

        assert(region.mX >= 0 && region.mY >= 0 && region.mX + region.mWidth <= getImage()->s()
            && region.mY + region.mHeight <= getImage()->t() && "painted outside the picture");

        // The bytes are written before the count moves, so a reader that saw the count sees the
        // paint.
        mRecent[mPaints % sRemembered] = region;
        ++mPaints;
        getImage()->dirty();
    }

    void PaintedTexture::paintAll()
    {
        paint(whole());
    }

    Painted PaintedTexture::since(const std::uint32_t seen) const
    {
        const std::uint32_t now = mPaints;
        const std::uint32_t behind = now - seen;
        if (behind == 0)
            return Painted{ .mPaints = now };

        if (behind > sRemembered)
            return Painted{ .mRegion = whole(), .mPaints = now };

        ImageRegion region;
        for (std::uint32_t paint = seen + 1; paint <= now; ++paint)
            region = region.joined(mRecent[(paint - 1) % sRemembered]);

        return Painted{ .mRegion = region, .mPaints = now };
    }

    ImageRegion PaintedTexture::whole() const
    {
        return ImageRegion{ 0, 0, getImage()->s(), getImage()->t() };
    }
}
