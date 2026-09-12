#pragma once

namespace Rtx
{
    /// What one frame has to reproject from, and whether anything read the answer. Asking is what
    /// spends the signal, so a pass added between two others cannot read a history and leave it
    /// unspent.
    class FrameHistory
    {
    public:
        /// @param basisLost the previous camera has no basis at all: a resize, a rebuild, the first
        ///        frame, or a walk through a door that no motion vector could have described.
        /// @param airStale,denoiserStale what `Renderer::resetHistory` set and no frame has spent.
        FrameHistory(const bool basisLost, const bool airStale, const bool denoiserStale)
            : mAirLost(airStale || basisLost)
            , mLost(denoiserStale || basisLost)
        {
        }

        /// The trace's, which is the fog volume's. Nothing records that this was read, because
        /// the trace runs on every frame and the caller spends the signal unconditionally.
        bool airLost() const { return mAirLost; }

        /// Whether a reconstruction has a past, for a pass about to read one. Spent by the frame
        /// that answers it and not by the frame that ends, so a `resetHistory` before an
        /// unfiltered frame is deferred to the frame that can act on it.
        ///
        /// @param reads whether the pass runs at all. False asks without spending anything.
        bool answer(const bool reads = true)
        {
            mAnswered = mAnswered || reads;
            return mLost;
        }

        /// Whether anything read `answer`, which is what lets the caller drop the signal.
        bool wasAnswered() const { return mAnswered; }

    private:
        bool mAirLost;
        bool mLost;
        bool mAnswered = false;
    };
}
