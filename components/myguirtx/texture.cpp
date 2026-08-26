#include "texture.hpp"

#include <cassert>
#include <cstring>

#include <algorithm>
#include <stdexcept>

#include <osg/Image>

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

    Texture::~Texture()
    {
        release();
    }

    void Texture::release()
    {
        if (mSlot != sNoSlot)
            mRenderer.dropGuiTexture(mSlot);

        mSlot = sNoSlot;
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
        mPixels.assign(static_cast<std::size_t>(width) * height * elements, 0);
        mSlot = mRenderer.addGuiTexture(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
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
        // that is most of them rather than read a `Vec4f` per pixel.
        MyGUIPlatform::writeRgba(*image, mPixels.data());

        upload();
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
        if (mSlot == sNoSlot)
            throw std::runtime_error("Texture is not created");
        if (mLocked)
            throw std::runtime_error("Texture already locked");

        mLocked = true;
        return mPixels.data();
    }

    void Texture::unlock()
    {
        if (!mLocked)
            throw std::runtime_error("Texture not locked");

        mLocked = false;
        upload();
    }

    void Texture::upload()
    {
        if (mNumElemBytes == 4)
        {
            mRenderer.writeGuiTexture(mSlot, whole(), mPixels);
            return;
        }

        // MyGUI asked for fewer channels than the table holds, so they are widened here rather than
        // by giving the table a second format to know about: a font atlas is written once and this
        // is the only place that knows what its bytes meant.
        const std::size_t count = static_cast<std::size_t>(mWidth) * mHeight;
        mWidened.resize(count * 4);

        for (std::size_t i = 0; i < count; ++i)
        {
            const std::uint8_t* in = mPixels.data() + i * mNumElemBytes;
            std::uint8_t* out = mWidened.data() + i * 4;

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

        mRenderer.writeGuiTexture(mSlot, whole(), mWidened);
    }

    void Texture::writeRegion(
        std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rows)
    {
        assert(mNumElemBytes == 4 && "a region write into a texture the GUI asked for fewer channels of");
        assert(x + width <= static_cast<std::uint32_t>(mWidth) && y + height <= static_cast<std::uint32_t>(mHeight)
            && "a region past the edge of the texture");
        assert(rows.size() == std::size_t{ width } * height * 4 && "the region's own rows, tightly packed");

        // **Kept here as well as sent**, because this side's copy is what MyGUI hands out on the next
        // `lock`: a picture written in part and then locked whole would otherwise come back holding
        // the region's old pixels.
        for (std::uint32_t row = 0; row < height; ++row)
            std::memcpy(mPixels.data() + (std::size_t{ y + row } * mWidth + x) * 4,
                rows.data() + std::size_t{ row } * width * 4, std::size_t{ width } * 4);

        mRenderer.writeGuiTexture(mSlot, Rtx::Renderer::GuiRegion{ x, y, width, height }, rows);
    }

    Rtx::Renderer::GuiRegion Texture::whole() const
    {
        return Rtx::Renderer::GuiRegion{ 0, 0, static_cast<std::uint32_t>(mWidth),
            static_cast<std::uint32_t>(mHeight) };
    }

    void Texture::setShader(const std::string& /*shaderName*/)
    {
        Log(Debug::Warning) << "Texture::setShader is not implemented";
    }
}
