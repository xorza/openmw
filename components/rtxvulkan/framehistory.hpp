#pragma once

namespace Rtx
{
    /// What one frame has to reproject from, and whether anything read the answer.
    ///
    /// **A state machine that was locals spread down a frame.** Some of them said what was lost and
    /// one recorded that something had asked, written wherever a pass happened to read one and read
    /// at the end — so a pass added between them could read a history and leave the signal unspent.
    /// Asking is what answers it here, so the two cannot come apart.
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

        /// The trace's, which is the fog volume's. **Nothing records that this was read**, because
        /// the trace runs on every frame and the caller spends the signal unconditionally.
        bool airLost() const { return mAirLost; }

        /// Whether a reconstruction has a past, for a pass about to read one.
        ///
        /// **Spent by the frame that answers it, not by the frame that ends.** Only a reconstruction
        /// carrying a past reads this, and a frame with neither denoiser carries none — so the
        /// signal has to survive such a frame. A `resetHistory` before an unfiltered frame would
        /// otherwise be dropped rather than deferred to the frame that can act on it.
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
