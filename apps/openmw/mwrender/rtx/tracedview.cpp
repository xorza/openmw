#include "tracedview.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>

#include <MyGUI_ITexture.h>
#include <MyGUI_RenderManager.h>
#include <osg/Image>

#include <components/myguiplatform/guirendermanager.hpp>
#include <components/myguirtx/texture.hpp>

#include "rtxrenderer.hpp"
#include "session.hpp"

namespace MWRender
{
    namespace
    {
        std::uint8_t channel(float value)
        {
            return static_cast<std::uint8_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
        }

        /// The trace behind one spec: of the world, or of the subtree the caller brought with it.
        ///
        /// **By value, and `Rtx::OffscreenTrace` neither copies nor moves.** Both returns are
        /// prvalues and so is the call, so guaranteed elision constructs it straight into the member
        /// — which is what lets the two constructors be the two kinds instead of a boolean.
        Rtx::OffscreenTrace makeTrace(const OffscreenViewSpec& spec, RtxRenderer& host, Rtx::Traversals& traversals)
        {
            const std::uint32_t width = static_cast<std::uint32_t>(spec.mWidth);
            const std::uint32_t height = static_cast<std::uint32_t>(spec.mHeight);
            Rtx::Renderer& renderer = host.getBackend();
            const std::uint32_t rays = rayMaskOf(spec.mMask);

            if (spec.mFromWorld)
                return Rtx::OffscreenTrace(renderer, width, height, rays);

            return Rtx::OffscreenTrace(renderer, width, height, rays, spec.mScene, spec.mMask, &traversals);
        }
    }

    TracedView::TracedView(const OffscreenViewSpec& spec, RtxRenderer& host, Rtx::Traversals& traversals)
        : mHost(host)
        , mTrace(makeTrace(spec, host, traversals))
        , mWidth(spec.mWidth)
        , mHeight(spec.mHeight)
    {
        mTrace.setFraming(spec.mFraming);
        mTrace.setLight(spec.mSun);
        mTrace.setClearColour(spec.mClearColour);

        // What `OffscreenView::getTexture` promises, and what the widgets showing one invert V for.
        mTrace.setRowOrder(Rtx::RowOrder::BottomFirst);

        mTexture
            = MyGUI::RenderManager::getInstance().createTexture(MyGUIPlatform::uniqueTextureName("rtx offscreen view"));
        mTexture->createManual(
            mWidth, mHeight, MyGUI::TextureUsage::Static | MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);

        // **The clear colour, before anything has been traced.** A view is shown from the frame it
        // is made on and drawn on some later one — a map tile is asked for as its cell arrives —
        // and the alternative is a widget holding whatever the slot was cleared to.
        const std::uint8_t colour[4] = { channel(spec.mClearColour.r()), channel(spec.mClearColour.g()),
            channel(spec.mClearColour.b()), channel(spec.mClearColour.a()) };

        auto* pixels = static_cast<std::uint8_t*>(mTexture->lock(MyGUI::TextureUsage::Write));
        for (int i = 0; i < mWidth * mHeight; ++i)
            std::memcpy(pixels + i * 4, colour, sizeof(colour));
        mTexture->unlock();

        mSlot = static_cast<MyGUIRtx::Texture*>(mTexture)->getSlot();
    }

    TracedView::~TracedView()
    {
        mHost.forgetView(*this);

        MyGUI::RenderManager::getInstance().destroyTexture(mTexture);
    }

    void TracedView::setExtent(int width, int height)
    {
        mTrace.setExtent(
            static_cast<std::uint32_t>(std::max(width, 1)), static_cast<std::uint32_t>(std::max(height, 1)));
    }

    void TracedView::sceneChanged()
    {
        // **Nothing either way, and for two different reasons.** A picture of the world is a picture
        // of the scene the mirror rebuilds every frame regardless; a picture of its own subject
        // walks that subject again on every `redraw`, which is the only time it is looked at.
    }

    void TracedView::redraw()
    {
        // Whatever is in the copy is a picture of the last redraw, and this is a new one.
        mCopyIsCurrent = false;
        mRedrawPending = true;

        mHost.redraw(*this);
    }

    void TracedView::draw()
    {
        if (!mTrace.isOfWorld())
        {
            const std::optional<PoseMoment> moment = mHost.describePose();
            if (!moment.has_value())
                return;

            if (!mTrace.rebuildSubject(moment->mStamp, moment->mFrame, moment->mImages))
                return;
        }

        mRedrawPending = false;
        mTrace.traceInto(mSlot, mKeepCopy);
    }

    void TracedView::keepCopy()
    {
        if (mKeepCopy)
            return;

        mKeepCopy = true;
        mCopy = new osg::Image;
        mCopy->allocateImage(mWidth, mHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        std::memset(mCopy->data(), 0, mCopy->getTotalSizeInBytes());
    }

    const osg::Image* TracedView::getCopy()
    {
        // Nothing while the redraw is queued and not yet recorded: the backend would hand over the
        // copy the last trace left, which landed, as if it were this one's.
        if (mCopy == nullptr || mRedrawPending)
            return nullptr;

        // **The whole texture and not the extent**, because the copy is what the global map paints
        // a cell from and a cell is the whole tile. Taken straight into the image the caller is
        // handed, the first time it is asked for after the trace that made it has landed.
        if (!mCopyIsCurrent)
            mCopyIsCurrent = mHost.getBackend().takeGuiCopy(
                mSlot, std::span<std::uint8_t>(mCopy->data(), mCopy->getTotalSizeInBytes()));

        return mCopyIsCurrent ? mCopy.get() : nullptr;
    }

}
