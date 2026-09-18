#include "paintedtexture.hpp"

#include <cassert>

#include <osg/GL>
#include <osg/State>

namespace SceneUtil
{
    class PaintedTexture::Uploader final : public osg::Texture2D::SubloadCallback
    {
    public:
        void load(const osg::Texture2D& texture, osg::State& state) const override
        {
            // This callback is installed by `PaintedTexture` alone, which is what makes the cast a
            // statement rather than a question.
            const auto& painted = static_cast<const PaintedTexture&>(texture);
            const osg::Image& image = *painted.getImage();

            glPixelStorei(GL_UNPACK_ALIGNMENT, image.getPacking());
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.s(), image.t(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());

            mSeen = painted.getPaintCount();
        }

        void subload(const osg::Texture2D& texture, osg::State& state) const override
        {
            const auto& painted = static_cast<const PaintedTexture&>(texture);
            const Painted pending = painted.since(mSeen);
            mSeen = pending.mPaints;
            if (pending.mRegion.empty())
                return;

            const osg::Image& image = *painted.getImage();
            const ImageRegion& region = pending.mRegion;

            // The rectangle out of the middle of the image's rows, which is what the row length
            // says; back to the default after, because the state object tracks neither.
            glPixelStorei(GL_UNPACK_ALIGNMENT, image.getPacking());
            glPixelStorei(GL_UNPACK_ROW_LENGTH, image.s());
            glTexSubImage2D(GL_TEXTURE_2D, 0, region.mX, region.mY, region.mWidth, region.mHeight, GL_RGBA,
                GL_UNSIGNED_BYTE, image.data(region.mX, region.mY));
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }

    private:
        /// One count and not one per context, because the game draws its interface through one
        /// context.
        mutable std::uint32_t mSeen = 0;
    };

    PaintedTexture::PaintedTexture(osg::Image* image)
    {
        assert(image != nullptr && image->getPixelFormat() == GL_RGBA && image->getDataType() == GL_UNSIGNED_BYTE
            && image->isDataContiguous() && "a painted picture is tightly packed RGBA8");

        setImage(image);
        setUnRefImageDataAfterApply(false);
        setResizeNonPowerOfTwoHint(false);
        setInternalFormat(GL_RGBA);
        setTextureSize(image->s(), image->t());
        setSubloadCallback(new Uploader);

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
