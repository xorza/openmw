#include "tracedview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <variant>

#include <MyGUI_ITexture.h>
#include <MyGUI_RenderFormat.h>
#include <osg/GL>
#include <osg/Image>
#include <osg/Vec3f>

#include <components/myguirtx/rendermanager.hpp>
#include <components/myguirtx/texture.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/mirrorpass.hpp>
#include <components/sceneutil/offscreenframing.hpp>

#include "classmasks.hpp"
#include "viewqueue.hpp"

namespace MWRender
{
    namespace
    {
        std::uint8_t channel(float value)
        {
            return static_cast<std::uint8_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
        }

        /// The spec as the trace takes it. Bottom row first, which is what
        /// `OffscreenView::getTexture` promises and what the widgets showing one invert V for.
        Rtx::ViewRequest requestFor(const OffscreenViewSpec& spec, ViewKind kind, Rtx::Traversals& traversals)
        {
            osg::Node* const subject = kind == ViewKind::Subject ? &spec.mScene : nullptr;
            return Rtx::ViewRequest{
                .mWidth = static_cast<std::uint32_t>(spec.mWidth),
                .mHeight = static_cast<std::uint32_t>(spec.mHeight),
                .mRayMask = rayMaskOf(spec.mMask),
                .mFraming = spec.mFraming,
                .mLight = spec.mSun,
                .mClear = spec.mClearColour,
                .mRowOrder = Rtx::RowOrder::BottomFirst,
                .mSubject = subject,
                .mSubjectMask = spec.mMask,
                .mTraversals = &traversals,
            };
        }

        /// MyGUI keys its textures by name, so each view names its own.
        std::string nextViewName()
        {
            static unsigned int next = 0;
            return "rtx offscreen view " + std::to_string(next++);
        }
    }

    TracedView::TracedView(const OffscreenViewSpec& spec, ViewKind kind, Rtx::Renderer& backend, ViewQueue& views,
        MyGUIRtx::RenderManager& gui, Rtx::Traversals& traversals)
        : mViews(views)
        , mTrace(backend, requestFor(spec, kind, traversals))
        , mTexture(gui.takeTexture(nextViewName()))
    {
        const int width = static_cast<int>(mTrace.getWidth());
        const int height = static_cast<int>(mTrace.getHeight());
        mTexture->createManual(
            width, height, MyGUI::TextureUsage::Static | MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);

        // **The clear colour, before anything has been traced.** A view is shown from the frame it
        // is made on and drawn on some later one — a map tile is asked for as its cell arrives —
        // and the alternative is a widget holding whatever the slot was cleared to.
        const std::uint8_t colour[4] = { channel(spec.mClearColour.r()), channel(spec.mClearColour.g()),
            channel(spec.mClearColour.b()), channel(spec.mClearColour.a()) };

        auto* pixels = static_cast<std::uint8_t*>(mTexture->lock(MyGUI::TextureUsage::Write));
        for (int i = 0; i < width * height; ++i)
            std::memcpy(pixels + i * 4, colour, sizeof(colour));
        mTexture->unlock();

        mViews.adopt(*this);
    }

    TracedView::~TracedView()
    {
        mViews.forget(*this);
    }

    bool TracedView::coversFromAbove(const osg::Vec2f& over) const
    {
        const auto* box = std::get_if<SceneUtil::Orthographic>(&mTrace.getFraming().mProjection);
        if (!isOfWorld() || box == nullptr)
            return false;

        // Where the eye stands is the inverse view's translation; the box is centred on it.
        const osg::Vec3f eye = osg::Matrixf::inverse(mTrace.getView()).getTrans();
        return std::abs(eye.x() - over.x()) <= box->mWidth * 0.5f
            && std::abs(eye.y() - over.y()) <= box->mHeight * 0.5f;
    }

    MyGUI::ITexture& TracedView::getTexture() const
    {
        return *mTexture;
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
        if (mCopyState != CopyState::NotWanted)
            mCopyState = CopyState::Queued;

        mViews.redraw(*this);
    }

    void TracedView::draw(const PoseMoment& moment)
    {
        if (!mTrace.isOfWorld())
        {
            if (!mTrace.rebuildSubject(moment.mStamp, moment.mStamp.getFrameNumber(), moment.mImages))
                return;
        }

        const bool keepCopy = mCopyState != CopyState::NotWanted;
        mTrace.traceInto(mTexture->getSlot(), keepCopy);
        if (keepCopy)
            mCopyState = CopyState::Recorded;
    }

    void TracedView::keepCopy()
    {
        if (mCopyState != CopyState::NotWanted)
            return;

        mCopyState = CopyState::Queued;
        mCopy = new osg::Image;
        mCopy->allocateImage(
            static_cast<int>(mTrace.getWidth()), static_cast<int>(mTrace.getHeight()), 1, GL_RGBA, GL_UNSIGNED_BYTE);
        std::memset(mCopy->data(), 0, mCopy->getTotalSizeInBytes());

        // The copy is left by the trace that is told to leave one, so a picture already traced
        // without it is traced again; one still queued carries it, and the host queues a view once.
        redraw();
    }

    const osg::Image* TracedView::getCopy()
    {
        // Nothing while the redraw is queued and not yet recorded: the backend would hand over the
        // copy the last trace left, which landed, as if it were this one's.
        //
        // **The whole texture and not the extent**, because the copy is what the global map paints
        // a cell from and a cell is the whole tile. Taken straight into the image the caller is
        // handed, the first time it is asked for after the trace that made it has landed.
        if (mCopyState == CopyState::Recorded
            && mTrace.takeCopy(
                mTexture->getSlot(), std::span<std::uint8_t>(mCopy->data(), mCopy->getTotalSizeInBytes())))
            mCopyState = CopyState::Taken;

        return mCopyState == CopyState::Taken ? mCopy.get() : nullptr;
    }

}
