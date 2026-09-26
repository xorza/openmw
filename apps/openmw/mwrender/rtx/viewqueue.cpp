#include "viewqueue.hpp"

#include <algorithm>
#include <cassert>

#include "tracedoverlay.hpp"
#include "tracedview.hpp"

namespace MWRender
{
    void ViewQueue::adopt(TracedView& view)
    {
        mViews.push_back(&view);
    }

    void ViewQueue::forget(TracedView& view)
    {
        std::erase(mViews, &view);
        std::erase(mDeferred, &view);

        // Nulled and not erased: a view can go from inside its own draw, while `draw` walks this
        // list, and replacing a pointer leaves that walk's iterators where they were.
        std::replace(mDrawing.begin(), mDrawing.end(), &view, static_cast<TracedView*>(nullptr));
    }

    void ViewQueue::redraw(TracedView& view)
    {
        if (std::find(mDeferred.begin(), mDeferred.end(), &view) == mDeferred.end())
            mDeferred.push_back(&view);
    }

    void ViewQueue::draw(const std::uint32_t worldViews, const osg::FrameStamp& posing)
    {
        assert(mDrawing.empty() && "a flush inside a flush");

        mDrawing.swap(mDeferred);
        mDeferred.clear();

        std::uint32_t world = 0;
        for (TracedView* view : mDrawing)
        {
            if (view == nullptr)
                continue;

            if (view->isOfWorld() && world == worldViews)
            {
                mDeferred.push_back(view);
                continue;
            }

            if (view->isOfWorld())
                ++world;

            view->draw(posing);
        }

        mDrawing.clear();
    }

    TracedView* ViewQueue::findWorldView(const osg::Vec2f& over) const
    {
        const auto found = std::find_if(
            mViews.begin(), mViews.end(), [&](const TracedView* view) { return view->coversFromAbove(over); });
        return found != mViews.end() ? *found : nullptr;
    }

    void ViewQueue::adoptOverlay(TracedOverlay& overlay)
    {
        mOverlays.push_back(&overlay);
    }

    void ViewQueue::forgetOverlay(TracedOverlay& overlay)
    {
        std::erase(mOverlays, &overlay);
    }

    void ViewQueue::finishOverlays()
    {
        for (TracedOverlay* overlay : mOverlays)
            overlay->finish();
    }
}
