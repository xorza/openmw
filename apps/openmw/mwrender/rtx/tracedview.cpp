#include "tracedview.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <variant>

#include <osg/Image>

#include <MyGUI_ITexture.h>
#include <MyGUI_RenderManager.h>

#include <components/myguirtx/texture.hpp>
#include <components/resource/resourcesystem.hpp>

#include "rtxrenderer.hpp"

namespace MWRender
{
    namespace
    {
        /// Names have to be unique in MyGUI's own table, and nothing outside ever looks one up.
        unsigned int sNextName = 0;

        std::uint8_t channel(float value)
        {
            return static_cast<std::uint8_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
        }

        /// The trace behind one spec: of the world, or of the subtree the caller brought with it.
        ///
        /// **By value, and `Rtx::OffscreenTrace` neither copies nor moves.** Both returns are
        /// prvalues and so is the call, so guaranteed elision constructs it straight into the member
        /// — which is what lets the two constructors be the two kinds instead of a boolean.
        Rtx::OffscreenTrace makeTrace(const OffscreenViewSpec& spec, RtxRenderer& owner, Rtx::Renderer& renderer)
        {
            const std::uint32_t width = static_cast<std::uint32_t>(spec.mWidth);
            const std::uint32_t height = static_cast<std::uint32_t>(spec.mHeight);

            if (spec.mFromWorld)
                return Rtx::OffscreenTrace(renderer, width, height);

            return Rtx::OffscreenTrace(renderer, width, height, spec.mScene, spec.mMask, &owner.getTraversals());
        }
    }

    TracedView::TracedView(const OffscreenViewSpec& spec, RtxRenderer& owner, Rtx::Renderer& renderer)
        : mOwner(owner)
        , mRenderer(renderer)
        , mTrace(makeTrace(spec, owner, renderer))
        , mWidth(spec.mWidth)
        , mHeight(spec.mHeight)
    {
        if (const auto* perspective = std::get_if<OffscreenViewSpec::Perspective>(&spec.mProjection))
            mTrace.setPerspective(perspective->mFieldOfView, spec.mNear, spec.mFar);
        else
        {
            const auto& box = std::get<OffscreenViewSpec::Orthographic>(spec.mProjection);
            mTrace.setOrthographic(box.mWidth, box.mHeight, spec.mNear, spec.mFar);
        }

        mTrace.setLight(spec.mSunDirection, spec.mSunDiffuse, spec.mSunAmbient);
        mTrace.setClearColour(spec.mClearColour);

        // What `OffscreenView::getTexture` promises, and what the widgets showing one invert V for.
        mTrace.setRowOrder(Rtx::RowOrder::BottomFirst);

        mTexture = MyGUI::RenderManager::getInstance().createTexture(std::format("rtx offscreen view {}", sNextName++));
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
        mOwner.forgetView(*this);

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

        if (mTrace.isOfWorld())
        {
            // **Asked for before there is a world, every time a game starts.** A cell asks for its
            // map tile as it loads, which is the frame before the one that first mirrors it; the
            // tile is drawn when there is something to draw it against rather than left blank until
            // the local map happens to ask again.
            if (!mOwner.hasScene())
            {
                mOwner.deferRedraw(*this);
                return;
            }
        }
        else
        {
            Resource::ResourceSystem* resources = mOwner.getResources();
            if (resources == nullptr)
                return;

            if (!mTrace.rebuildSubject(mOwner.getUpdateStamp(), mOwner.getFrame(), *resources->getImageManager()))
                return;
        }

        mTrace.traceInto(mSlot);

        if (!mKeepCopy)
            return;

        // **The whole texture and not the extent**, because the copy is what the global map paints
        // a cell from and a cell is the whole tile. The read is the only time a picture inside the
        // interface comes back to main memory, which is why it is asked for rather than always done.
        mRenderer.readGuiTexture(mSlot, mPixels);
        std::memcpy(mCopy->data(), mPixels.data(), std::min<std::size_t>(mPixels.size(), mCopy->getTotalSizeInBytes()));

        mCopyIsCurrent = true;
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

    const osg::Image* TracedView::getCopy() const
    {
        return mCopyIsCurrent ? mCopy.get() : nullptr;
    }

}
