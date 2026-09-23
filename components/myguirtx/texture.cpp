#include "texture.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include <MyGUI_RenderFormat.h>
#include <osg/Image>
#include <osg/ref_ptr>

#include <components/resource/imagemanager.hpp>
#include <components/rtx/guirenderer.hpp>
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

    Texture::Texture(std::string name, Rtx::GuiRenderer& renderer, Resource::ImageManager* imageManager)
        : SlotTexture(std::move(name), renderer)
        , mImageManager(imageManager)
    {
    }

    void Texture::release()
    {
        drop();
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

        mFormat = format;
        mUsage = usage;
        mNumElemBytes = elements;
        take(width, height);

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
        // (`docs/rtx/architecture.md`, section 2), and a second decoder here would be a second set
        // of formats to be wrong about.
        const osg::ref_ptr<osg::Image> image = mImageManager->getImage(VFS::Path::Normalized(fname));

        createManual(image->s(), image->t(), MyGUI::TextureUsage::Static, MyGUI::PixelFormat::R8G8B8A8);
        sendImage(*image);
    }

    void Texture::destroy()
    {
        release();
    }

    void* Texture::lock(MyGUI::TextureUsage /*access*/)
    {
        if (getSlot().isNone())
            throw std::runtime_error("Texture is not created");
        if (mLocked)
            throw std::runtime_error("Texture already locked");

        mLocked = true;

        return mNumElemBytes == 4 ? mRenderer.lendGuiTexture(getSlot(), whole()).data() : mPixels.data();
    }

    void Texture::unlock()
    {
        if (!mLocked)
            throw std::runtime_error("Texture not locked");

        mLocked = false;

        if (mNumElemBytes == 4)
            mRenderer.sendGuiTexture(getSlot());
        else
            widen();
    }

    void Texture::widen()
    {
        // MyGUI asked for fewer channels than the table holds, so they are widened here rather than
        // by giving the table a second format to know about: a font atlas is written once and this
        // is the only place that knows what its bytes meant.
        const std::size_t count = static_cast<std::size_t>(getWidth()) * getHeight();
        std::uint8_t* const into = mRenderer.lendGuiTexture(getSlot(), whole()).data();

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

        mRenderer.sendGuiTexture(getSlot());
    }
}
