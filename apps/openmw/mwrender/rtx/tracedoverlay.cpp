#include "tracedoverlay.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

#include <osg/Image>

#include <components/myguirtx/rendermanager.hpp>
#include <components/sceneutil/paintedtexture.hpp>

#include "../offscreenview.hpp"
#include "../pixels.hpp"
#include "viewqueue.hpp"

namespace MWRender
{
    TracedOverlay::TracedOverlay(const MapOverlaySpec& spec, ViewQueue& views, MyGUIRtx::RenderManager& gui)
        : mViews(views)
        , mLandAlpha(spec.mLandAlpha)
    {
        mImage = new osg::Image;
        mImage->allocateImage(spec.mWidth, spec.mHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        assert(mImage->isDataContiguous());
        std::memset(mImage->data(), 0, mImage->getTotalSizeInBytes());

        mTexture = new SceneUtil::PaintedTexture(mImage);
        mGuiTexture = gui.shareTexture(*mTexture);

        mViews.adoptOverlay(*this);
    }

    TracedOverlay::~TracedOverlay()
    {
        mViews.forgetOverlay(*this);
    }

    void TracedOverlay::paintTile(const SceneUtil::ImageRegion& destination, std::shared_ptr<OffscreenView> tile)
    {
        // Asked for here and read at `finish`: the copy costs a transfer off the device per redraw
        // of the tile from now on, which is the price upstream paid to copy the overlay back.
        tile->keepCopy();

        // The last word for a rectangle wins, so a cell crossed twice before its picture came back
        // is painted once, from the later tile.
        const auto same = std::find_if(mPending.begin(), mPending.end(),
            [&](const Pending& pending) { return pending.mDestination == destination; });
        if (same != mPending.end())
            same->mTile = std::move(tile);
        else
            mPending.push_back(Pending{ .mDestination = destination, .mTile = std::move(tile) });
    }

    void TracedOverlay::finish()
    {
        std::erase_if(mPending, [&](Pending& pending) {
            // A tile nothing else holds is one the local map let go of before it was drawn, and
            // nothing will draw it now: kept, it would be polled for ever and hold its slot.
            if (pending.mTile.use_count() == 1)
                return true;

            const osg::Image* drawn = pending.mTile->getCopy();
            if (drawn == nullptr)
                return false;

            composite(pending.mDestination, *drawn);
            return true;
        });
    }

    void TracedOverlay::composite(const SceneUtil::ImageRegion& destination, const osg::Image& tile)
    {
        if (compositeTile(tile, *mLandAlpha, *mImage, destination, mCellScratch))
            mTexture->paint(destination);
    }

    void TracedOverlay::paintImage(
        const SceneUtil::ImageRegion& destination, osg::ref_ptr<osg::Image> image, const SceneUtil::ImageRegion& source)
    {
        mPending.clear();
        std::memset(mImage->data(), 0, mImage->getTotalSizeInBytes());

        const osg::ref_ptr<osg::Image> rgba = asRgba(std::move(image));
        resampleRegion(*rgba, source, *mImage, destination);

        mTexture->paintAll();
    }

    void TracedOverlay::replace(osg::ref_ptr<osg::Image> image)
    {
        mPending.clear();

        // Into the overlay's own image, so what the texture and its mirror draw from never changes
        // identity.
        const osg::ref_ptr<osg::Image> rgba = asRgba(std::move(image));
        assert(rgba->s() == mImage->s() && rgba->t() == mImage->t());
        std::memcpy(mImage->data(), rgba->data(), mImage->getTotalSizeInBytes());

        mTexture->paintAll();
    }

    void TracedOverlay::clear()
    {
        mPending.clear();
        std::memset(mImage->data(), 0, mImage->getTotalSizeInBytes());
        mTexture->paintAll();
    }
}
