#pragma once

#include <cstdint>
#include <vector>

#include <osg/Vec2f>

namespace MWRender
{
    class TracedView;

    /// The pictures inside the interface that the ray tracer holds, and the ones asked for since
    /// the last frame: a view is drawn in the next frame's window, after the world's placement and
    /// before its trace, where the copy of the tables a picture reads is the frame's own. Drawn
    /// where asked, a picture made a wait of every placement that could still follow it. The views
    /// are borrowed: the caller owns every one, and `forget` keeps that sound.
    class ViewQueue
    {
    public:
        /// A view made, from its constructor: kept on the list of the pictures alive, which
        /// `findWorldView` answers from.
        void adopt(TracedView& view);

        /// Takes a view off every list, because it is going away — the one being drawn included.
        void forget(TracedView& view);

        /// Queues `view` for the next flush. Asked twice in one frame is drawn once.
        void redraw(TracedView& view);

        /// Draws the pictures asked for since the last flush: every subject's and up to
        /// `worldViews` of the world's, the rest waiting for the next. A fresh load asks for nine
        /// map tiles at once and a cell crossing for a row of three; a picture of a subject is
        /// never held back.
        void draw(std::uint32_t worldViews);

        /// Whether a flush is in progress, which a flush inside a flush must not be.
        bool isDrawing() const { return !mDrawing.empty(); }

        bool hasDeferred() const { return !mDeferred.empty(); }

        /// The picture of the world taken straight down over `over`, or null where none is alive:
        /// the local map's tile of the cell the point is in, found by the renderer that drew it.
        TracedView* findWorldView(const osg::Vec2f& over) const;

    private:
        /// Every picture alive, in the order made.
        std::vector<TracedView*> mViews;

        /// Pictures asked for and not yet drawn, in the order asked.
        std::vector<TracedView*> mDeferred;

        /// The list a flush walks, swapped out of `mDeferred` so a redraw cannot grow what is being
        /// iterated. Kept, because this sits on the frame path.
        std::vector<TracedView*> mDrawing;
    };
}
