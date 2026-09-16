#include "slottexture.hpp"

#include <utility>

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

    void SlotTexture::saveToFile(const std::string& fname)
    {
        Log(Debug::Warning) << "Would save image to file " << fname;
    }

    void SlotTexture::setShader(const std::string& /*shaderName*/)
    {
        Log(Debug::Warning) << "Texture::setShader is not implemented";
    }
}
