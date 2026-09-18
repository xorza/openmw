#include "paintedmirror.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <osg/Image>

#include <components/rtx/guirenderer.hpp>
#include <components/sceneutil/paintedtexture.hpp>

namespace MyGUIRtx
{
    PaintedMirror::PaintedMirror(Rtx::GuiRenderer& renderer, SceneUtil::PaintedTexture& source)
        : MirrorTexture(renderer)
        , mSource(&source)
    {
        refresh();
    }

    PaintedMirror::~PaintedMirror() = default;

    void PaintedMirror::refresh()
    {
        const osg::Image& image = *mSource->getImage();
        if (getSlot().isNone())
        {
            take(image.s(), image.t());
            mSent = false;
        }

        const SceneUtil::Painted pending = mSource->since(mSeen);
        const SceneUtil::ImageRegion region = mSent ? pending.mRegion : mSource->whole();
        mSeen = pending.mPaints;
        if (region.empty())
            return;

        const Rtx::GuiRegion rows{ static_cast<std::uint32_t>(region.mX), static_cast<std::uint32_t>(region.mY),
            static_cast<std::uint32_t>(region.mWidth), static_cast<std::uint32_t>(region.mHeight) };
        std::uint8_t* into = mRenderer.lendGuiTexture(getSlot(), rows).data();

        // A row of the rectangle at a time: the picture is RGBA8 and packed, which the source
        // asserts, so a row is one copy.
        const std::size_t rowBytes = static_cast<std::size_t>(region.mWidth) * 4;
        for (int row = 0; row < region.mHeight; ++row)
            std::memcpy(
                into + rowBytes * static_cast<std::size_t>(row), image.data(region.mX, region.mY + row), rowBytes);

        mRenderer.sendGuiTexture(getSlot());
        mSent = true;
    }
}
