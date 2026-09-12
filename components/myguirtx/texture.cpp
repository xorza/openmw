#include "texture.hpp"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <osg/Image>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/pixels.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/rtx/renderer.hpp>
#include <components/vfs/pathutil.hpp>

namespace MyGUIRtx
{
    namespace
    {
        std::size_t elementsOf(MyGUI::PixelFormat format)
        {
            switch (format.getValue())
            {
                case MyGUI::PixelFormat::L8:
                    return 1;
                case MyGUI::PixelFormat::L8A8:
                    return 2;
                case MyGUI::PixelFormat::R8G8B8:
                    return 3;
                case MyGUI::PixelFormat::R8G8B8A8:
                    return 4;
                default:
                    return 0;
            }
        }
    }

    Texture::Texture(std::string name, Rtx::Renderer& renderer, Resource::ImageManager* imageManager)
        : mName(std::move(name))
        , mRenderer(renderer)
        , mImageManager(imageManager)
    {
    }

    Texture::Texture(Rtx::Renderer& renderer, osg::Texture2D& source)
        : mRenderer(renderer)
        , mImageManager(nullptr)
        , mFormat(MyGUI::PixelFormat::R8G8B8A8)
        , mUsage(MyGUI::TextureUsage::Static)
        , mNumElemBytes(4)
        , mSource(&source)
    {
        refresh();
    }

    Texture::~Texture()
    {
        release();
    }

    void Texture::release()
    {
        if (!mSlot.isNone())
            mRenderer.dropGuiTexture(mSlot);

        mSlot = Rtx::GuiSlot::none();
        mWidth = 0;
        mHeight = 0;
        mFormat = MyGUI::PixelFormat::Unknow;
        mUsage = MyGUI::TextureUsage::Default;
        mNumElemBytes = 0;
        mPixels.clear();
        mLocked = false;
    }

    void Texture::createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format)
    {
        const std::size_t elements = elementsOf(format);
        if (elements == 0)
            throw std::runtime_error("Texture format not supported");

        release();

        mWidth = width;
        mHeight = height;
        mFormat = format;
        mUsage = usage;
        mNumElemBytes = elements;
        mSlot = mRenderer.addGuiTexture(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));

        // Nothing at four channels: `lock` hands out the renderer's own bytes there, and this is
        // only what a narrower format is widened out of.
        if (elements != 4)
            mPixels.assign(static_cast<std::size_t>(width) * height * elements, 0);
    }

    void Texture::loadFromFile(const std::string& fname)
    {
        if (mImageManager == nullptr)
            throw std::runtime_error("No imagemanager set");

        // **Decoded by the engine's own image manager**, which is where every other picture in this
        // fork comes from: OpenSceneGraph stays as the content loader whatever draws
        // (`CLAUDE.md`), and a second decoder here would be a second set of formats to be wrong
        // about.
        const osg::ref_ptr<osg::Image> image = mImageManager->getImage(VFS::Path::Normalized(fname));

        createManual(image->s(), image->t(), MyGUI::TextureUsage::Static, MyGUI::PixelFormat::R8G8B8A8);

        // Widened by the same code the other backend widens with, which knows to `memcpy` the case
        // that is most of them rather than read a `Vec4f` per pixel — and it writes each pixel once
        // and in order, which is what the renderer's own bytes want.
        MyGUIPlatform::writeRgba(*image, mRenderer.lendGuiTexture(mSlot, whole()).data());

        mRenderer.sendGuiTexture(mSlot);
    }

    void Texture::saveToFile(const std::string& fname)
    {
        Log(Debug::Warning) << "Would save image to file " << fname;
    }

    void Texture::destroy()
    {
        release();
    }

    void* Texture::lock(MyGUI::TextureUsage /*access*/)
    {
        if (mSlot.isNone())
            throw std::runtime_error("Texture is not created");
        if (mLocked)
            throw std::runtime_error("Texture already locked");

        mLocked = true;

        return mNumElemBytes == 4 ? mRenderer.lendGuiTexture(mSlot, whole()).data() : mPixels.data();
    }

    void Texture::unlock()
    {
        if (!mLocked)
            throw std::runtime_error("Texture not locked");

        mLocked = false;

        if (mNumElemBytes == 4)
            mRenderer.sendGuiTexture(mSlot);
        else
            widen();
    }

    void Texture::widen()
    {
        // MyGUI asked for fewer channels than the table holds, so they are widened here rather than
        // by giving the table a second format to know about: a font atlas is written once and this
        // is the only place that knows what its bytes meant.
        const std::size_t count = static_cast<std::size_t>(mWidth) * mHeight;
        std::uint8_t* const into = mRenderer.lendGuiTexture(mSlot, whole()).data();

        for (std::size_t i = 0; i < count; ++i)
        {
            const std::uint8_t* in = mPixels.data() + i * mNumElemBytes;
            std::uint8_t* out = into + i * 4;

            switch (mNumElemBytes)
            {
                case 1:
                    out[0] = out[1] = out[2] = in[0];
                    out[3] = 0xFF;
                    break;
                case 2:
                    out[0] = out[1] = out[2] = in[0];
                    out[3] = in[1];
                    break;
                default:
                    out[0] = in[0];
                    out[1] = in[1];
                    out[2] = in[2];
                    out[3] = 0xFF;
                    break;
            }
        }

        mRenderer.sendGuiTexture(mSlot);
    }

    void Texture::refresh()
    {
        if (mSource == nullptr)
            return;

        const osg::Image* const image = mSource->getImage();
        if (image == nullptr || image->s() <= 0 || image->t() <= 0)
            return;

        if (image == mSeen && image->getModifiedCount() == mSeenCount)
            return;

        // A picture of another shape is another slot: the table sizes a slot once.
        if (mSlot.isNone() || image->s() != mWidth || image->t() != mHeight)
        {
            if (!mSlot.isNone())
                mRenderer.dropGuiTexture(mSlot);

            mWidth = image->s();
            mHeight = image->t();
            mSlot = mRenderer.addGuiTexture(static_cast<std::uint32_t>(mWidth), static_cast<std::uint32_t>(mHeight));
            mLastSent.clear();
        }

        // The run of rows that differ from what was sent, against the image's own bytes rather
        // than the widened ones: the comparison is over the picture as the game wrote it, and
        // widening is paid for the rows that go. The same image with a moved count is the fog of
        // war or the world map, written in a corner; another image under the texture is a video
        // frame, and goes whole.
        const std::size_t rowBytes = image->getRowSizeInBytes();
        const std::size_t total = rowBytes * static_cast<std::size_t>(mHeight);
        int first = 0;
        int last = mHeight - 1;
        if (image == mSeen && image->isDataContiguous() && mLastSent.size() == total)
        {
            while (
                first <= last && std::memcmp(image->data(0, first), mLastSent.data() + rowBytes * first, rowBytes) == 0)
                ++first;
            while (last > first && std::memcmp(image->data(0, last), mLastSent.data() + rowBytes * last, rowBytes) == 0)
                --last;
        }

        mSeen = image;
        mSeenCount = image->getModifiedCount();

        if (first > last)
            return;

        const std::uint32_t count = static_cast<std::uint32_t>(last - first + 1);
        const Rtx::GuiRegion rows{ 0, static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(mWidth), count };
        MyGUIPlatform::writeRgbaRows(
            *image, first, static_cast<int>(count), mRenderer.lendGuiTexture(mSlot, rows).data());
        mRenderer.sendGuiTexture(mSlot);

        if (image->isDataContiguous())
        {
            mLastSent.resize(total);
            std::memcpy(mLastSent.data() + rowBytes * first, image->data(0, first), rowBytes * count);
        }
        else
            mLastSent.clear();
    }

    Rtx::GuiRegion Texture::whole() const
    {
        return Rtx::GuiRegion{ 0, 0, static_cast<std::uint32_t>(mWidth), static_cast<std::uint32_t>(mHeight) };
    }

    void Texture::setShader(const std::string& /*shaderName*/)
    {
        Log(Debug::Warning) << "Texture::setShader is not implemented";
    }
}
