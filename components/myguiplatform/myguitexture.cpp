#include "myguitexture.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

#include <osg/State>
#include <osg/StateSet>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>

namespace MyGUIPlatform
{

    void OSGTexture::DirtyRows::add(int first, int count)
    {
        if (count <= 0)
            return;

        if (empty())
        {
            mFirst = first;
            mCount = count;
            return;
        }

        const int last = std::max(mFirst + mCount, first + count);
        mFirst = std::min(mFirst, first);
        mCount = last - mFirst;
    }

    /// Uploads the picture, and after the first time only the rows that changed.
    ///
    /// **`apply` calls this on every draw once the texture object exists**, which is why an empty run
    /// is the ordinary case and costs a comparison. A texture with a subload callback is never
    /// uploaded by OpenSceneGraph itself, so what the image says is only ever sent from here.
    class OSGTexture::Subload : public osg::Texture2D::SubloadCallback
    {
    public:
        void load(const osg::Texture2D& texture, osg::State& state) const override
        {
            const osg::Image* image = texture.getImage();

            // **Whatever was left bound is not where these bytes are.** OpenSceneGraph's own upload
            // path binds a pixel buffer where the image has one and unbinds it where it has not;
            // this image has none, and a client pointer read as an offset into somebody else's
            // buffer is `GL_INVALID_OPERATION` on the frames a cell arrives and nothing on the rest.
            state.unbindPixelBufferObject();

            texture.setNumMipmapLevels(1);

            glPixelStorei(GL_UNPACK_ALIGNMENT, image->getPacking());
            glTexImage2D(GL_TEXTURE_2D, 0, texture.getInternalFormat(), image->s(), image->t(), 0,
                image->getPixelFormat(), image->getDataType(), image->data());

            mRows.clear();
        }

        void subload(const osg::Texture2D& texture, osg::State& state) const override
        {
            if (mRows.empty())
                return;

            const osg::Image* image = texture.getImage();

            state.unbindPixelBufferObject();

            // Whole rows, so what goes is one run of the image's own bytes and the driver is told
            // nothing about where the rest of it is.
            glPixelStorei(GL_UNPACK_ALIGNMENT, image->getPacking());
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, mRows.mFirst, image->s(), mRows.mCount, image->getPixelFormat(),
                image->getDataType(), image->data(0, mRows.mFirst));

            mRows.clear();
        }

        void add(int first, int count) { mRows.add(first, count); }

        DirtyRows getRows() const { return mRows; }

    private:
        /// Written by the update traversal and consumed by the draw one, which is safe for the
        /// reason writing the image is: until the texture has been drawn with, nothing reads it, and
        /// after that the texture is `DYNAMIC` and the two traversals no longer overlap on it.
        mutable DirtyRows mRows;
    };

    OSGTexture::OSGTexture(const std::string& name, Resource::ImageManager* imageManager)
        : mName(name)
        , mImageManager(imageManager)
        , mFormat(MyGUI::PixelFormat::Unknow)
        , mUsage(MyGUI::TextureUsage::Default)
        , mNumElemBytes(0)
        , mWidth(0)
        , mHeight(0)
    {
    }

    OSGTexture::OSGTexture(osg::Texture2D* texture, osg::StateSet* injectState)
        : mImageManager(nullptr)
        , mTexture(texture)
        , mInjectState(injectState)
        , mFormat(MyGUI::PixelFormat::Unknow)
        , mUsage(MyGUI::TextureUsage::Default)
        , mNumElemBytes(0)
        , mWidth(texture->getTextureWidth())
        , mHeight(texture->getTextureHeight())
    {
    }

    OSGTexture::~OSGTexture() {}

    void OSGTexture::createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format)
    {
        GLenum glfmt = GL_NONE;
        size_t numelems = 0;
        switch (format.getValue())
        {
            case MyGUI::PixelFormat::L8:
                glfmt = GL_LUMINANCE;
                numelems = 1;
                break;
            case MyGUI::PixelFormat::L8A8:
                glfmt = GL_LUMINANCE_ALPHA;
                numelems = 2;
                break;
            case MyGUI::PixelFormat::R8G8B8:
                glfmt = GL_RGB;
                numelems = 3;
                break;
            case MyGUI::PixelFormat::R8G8B8A8:
                glfmt = GL_RGBA;
                numelems = 4;
                break;
        }
        if (glfmt == GL_NONE)
            throw std::runtime_error("Texture format not supported");

        mWidth = width;
        mHeight = height;
        mFormat = format;
        mUsage = usage;
        mNumElemBytes = numelems;

        // **Allocated once, and this is the buffer `lock` hands out.** Nothing here is reallocated
        // again until the picture changes shape, which is what makes a video frame a `memcpy`.
        mImage = new osg::Image;
        mImage->allocateImage(width, height, 1, glfmt, GL_UNSIGNED_BYTE);

        // A texture that has never been drawn with is one nothing can be reading, whatever the last
        // one was.
        mLocked = false;
        mDrawn = false;
        mDynamic = false;

        makeTexture();
    }

    OSGTexture::Subload* OSGTexture::getSubload() const
    {
        if (!mTexture.valid())
            return nullptr;

        return static_cast<Subload*>(mTexture->getSubloadCallback());
    }

    void OSGTexture::makeTexture()
    {
        mTexture = new osg::Texture2D;
        mTexture->setTextureSize(mWidth, mHeight);
        mTexture->setSourceFormat(mImage->getPixelFormat());
        mTexture->setSourceType(mImage->getDataType());
        // Otherwise `apply` rewrites the texture's own dimensions to the nearest power of two and
        // rescales the image to match, which `lock` would then hand out as the buffer size — every
        // upload after the first would fill part of a bigger buffer and leave the rest undefined.
        mTexture->setResizeNonPowerOfTwoHint(false);

        mTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

        // **Carried but never uploaded by OpenSceneGraph**, which the subload callback is what
        // decides: the image is what says the internal format and what the callback reads, and the
        // callback is what sends it. Kept after apply for the same reason — it is the picture, not a
        // staging copy of it.
        mTexture->setImage(mImage);
        mTexture->setUnRefImageDataAfterApply(false);
        mTexture->setSubloadCallback(new Subload);
    }

    void OSGTexture::destroy()
    {
        mImage = nullptr;
        mTexture = nullptr;
        mFormat = MyGUI::PixelFormat::Unknow;
        mUsage = MyGUI::TextureUsage::Default;
        mNumElemBytes = 0;
        mWidth = 0;
        mHeight = 0;
        mLocked = false;
        mDrawn = false;
        mDynamic = false;
    }

    void OSGTexture::loadFromFile(const std::string& fname)
    {
        if (!mImageManager)
            throw std::runtime_error("No imagemanager set");

        osg::ref_ptr<osg::Image> image(mImageManager->getImage(VFS::Path::Normalized(fname)));
        mTexture = new osg::Texture2D(image);
        mTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setTextureWidth(image->s());
        mTexture->setTextureHeight(image->t());
        // disable mip-maps
        mTexture->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);

        mWidth = image->s();
        mHeight = image->t();

        mUsage = MyGUI::TextureUsage::Static;
    }

    void OSGTexture::saveToFile(const std::string& fname)
    {
        Log(Debug::Warning) << "Would save image to file " << fname;
    }

    void* OSGTexture::lock(MyGUI::TextureUsage /*access*/)
    {
        if (!mImage.valid())
            throw std::runtime_error("Texture is not created");
        if (mLocked)
            throw std::runtime_error("Texture already locked");

        // **Before the buffer is handed out and not after it.** By the time `unlock` is called the
        // caller has already written the picture, and if that picture is one the draw traversal may
        // still be reading, the write was the race.
        promote();

        mLocked = true;
        return mImage->data();
    }

    void OSGTexture::unlock()
    {
        if (!mLocked)
            throw std::runtime_error("Texture not locked");

        mLocked = false;
        getSubload()->add(0, mHeight);
    }

    void OSGTexture::writeRegion(
        std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rows)
    {
        assert(mNumElemBytes == 4 && "a region write into a texture the GUI asked for fewer channels of");
        assert(x + width <= static_cast<std::uint32_t>(mWidth) && y + height <= static_cast<std::uint32_t>(mHeight)
            && "a region past the edge of the texture");
        assert(rows.size() == std::size_t{ width } * height * 4 && "the region's own rows, tightly packed");

        promote();

        std::uint8_t* pixels = mImage->data();
        for (std::uint32_t row = 0; row < height; ++row)
            std::memcpy(pixels + (std::size_t{ y + row } * mWidth + x) * 4,
                rows.data() + std::size_t{ row } * width * 4, std::size_t{ width } * 4);

        getSubload()->add(static_cast<int>(y), static_cast<int>(height));
    }

    void OSGTexture::promote()
    {
        if (mDynamic || !mDrawn)
            return;

        // The picture as it stands, copied, because the draw traversal of the frame that batched it
        // may still be reading the one it is copied from. Both it and the texture over it stay alive
        // as long as that batch does, which is what `Drawable::Batch` holding a `ref_ptr` is for.
        osg::ref_ptr<osg::Image> fresh = new osg::Image;
        fresh->allocateImage(mWidth, mHeight, 1, mImage->getPixelFormat(), mImage->getDataType());
        std::memcpy(fresh->data(), mImage->data(), mImage->getTotalSizeInBytes());

        mImage = fresh;
        makeTexture();

        // **The whole of what makes every write after this one safe in place.** `doRender` reads it
        // off the texture and marks the drawable with it, and `osgViewer` will not start another
        // update traversal until a dynamic object has been drawn.
        mTexture->setDataVariance(osg::Object::DYNAMIC);
        mDynamic = true;
    }

    OSGTexture::DirtyRows OSGTexture::getDirtyRows() const
    {
        const Subload* subload = getSubload();
        return subload != nullptr ? subload->getRows() : DirtyRows{};
    }

    // Render-to-texture not currently implemented.
    MyGUI::IRenderTarget* OSGTexture::getRenderTarget()
    {
        return nullptr;
    }

    void OSGTexture::setShader(const std::string& /*shaderName*/)
    {
        Log(Debug::Warning) << "OSGTexture::setShader is not implemented";
    }
}
