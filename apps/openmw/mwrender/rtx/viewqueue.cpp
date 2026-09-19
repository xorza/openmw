#include "viewqueue.hpp"

#include <algorithm>
#include <cassert>

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

        // Nulled and not erased: a view can go from inside its own draw, and the loop below is
        // walking this list by index.
        std::replace(mDrawing.begin(), mDrawing.end(), &view, static_cast<TracedView*>(nullptr));
    }

    void ViewQueue::redraw(TracedView& view)
    {
        if (std::find(mDeferred.begin(), mDeferred.end(), &view) == mDeferred.end())
            mDeferred.push_back(&view);
    }

    void ViewQueue::draw(const std::uint32_t worldViews)
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

            view->draw();
        }

        mDrawing.clear();
    }

    TracedView* ViewQueue::findWorldView(const osg::Vec2f& over) const
    {
        const auto found = std::find_if(
            mViews.begin(), mViews.end(), [&](const TracedView* view) { return view->coversFromAbove(over); });
        return found != mViews.end() ? *found : nullptr;
    }
}
