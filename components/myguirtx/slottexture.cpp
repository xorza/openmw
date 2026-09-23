#include "slottexture.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <osg/GL>
#include <osg/Image>
#include <osg/Vec4f>

#include <components/debug/debuglog.hpp>

namespace MyGUIRtx
{
    SlotTexture::SlotTexture(std::string name, Rtx::GuiRenderer& renderer)
        : mRenderer(renderer)
        , mName(std::move(name))
    {
    }

    SlotTexture::~SlotTexture()
    {
        drop();
    }

    void SlotTexture::take(const int width, const int height)
    {
        drop();

        mWidth = width;
        mHeight = height;
        mSlot = mRenderer.addGuiTexture(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    }

    void SlotTexture::drop()
    {
        if (!mSlot.isNone())
            mRenderer.dropGuiTexture(mSlot);

        mSlot = Rtx::GuiSlot::none();
        mWidth = 0;
        mHeight = 0;
    }

    Rtx::GuiRegion SlotTexture::whole() const
    {
        return Rtx::GuiRegion{ 0, 0, static_cast<std::uint32_t>(mWidth), static_cast<std::uint32_t>(mHeight) };
    }

    void SlotTexture::sendImage(const osg::Image& image)
    {
        assert(image.s() == mWidth && image.t() == mHeight && "an image sent into a slot of another size");

        std::uint8_t* into = mRenderer.lendGuiTexture(mSlot, whole()).data();
        const std::size_t bytes = static_cast<std::size_t>(mWidth) * mHeight * 4;
        if (image.getPixelFormat() == GL_RGBA && image.getDataType() == GL_UNSIGNED_BYTE && image.isDataContiguous()
            && image.getTotalSizeInBytes() == bytes)
            std::memcpy(into, image.data(), bytes);
        else
        {
            for (int y = 0; y < mHeight; ++y)
                for (int x = 0; x < mWidth; ++x, into += 4)
                {
                    const osg::Vec4f colour = image.getColor(x, y);
                    into[0] = static_cast<std::uint8_t>(std::clamp(colour.r(), 0.f, 1.f) * 255.f + 0.5f);
                    into[1] = static_cast<std::uint8_t>(std::clamp(colour.g(), 0.f, 1.f) * 255.f + 0.5f);
                    into[2] = static_cast<std::uint8_t>(std::clamp(colour.b(), 0.f, 1.f) * 255.f + 0.5f);
                    into[3] = static_cast<std::uint8_t>(std::clamp(colour.a(), 0.f, 1.f) * 255.f + 0.5f);
                }
        }

        mRenderer.sendGuiTexture(mSlot);
    }

    void SlotTexture::saveToFile(const std::string& fname)
    {
        Log(Debug::Warning) << "Would save image to file " << fname;
    }

    void SlotTexture::setShader(const std::string& /*shaderName*/)
    {
        Log(Debug::Warning) << "Texture::setShader is not implemented";
    }
}
