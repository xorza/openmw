#pragma once

#include <algorithm>
#include <cstdint>

#include "timeline.hpp"

namespace Rtx
{
    /// The last submit that names a resource, as the value it signals on the queue's timeline —
    /// what a host write of the resource waits for and asserts against, because a host write over
    /// a submit still reading is the one hazard the layers cannot see. Held by a buffer, and by a
    /// descriptor set, which is bound by handle and has no buffer to carry one.
    ///
    /// **Stamped at the hand-out and nowhere else.** An address, a descriptor, a copy's ends and a
    /// set bound each name their resource for the next submit as they hand it out, so the stamp is
    /// exact for every reader on the queue whatever carried it — a frame's trace, a placement, a
    /// deferred picture riding the interface's own submit. A count kept beside it in frames was
    /// exact for the first of those and wrong for the rest.
    ///
    /// Mutable throughout, because naming is not a change to the bytes and every caller that names
    /// holds the resource const.
    class ReadStamp
    {
    public:
        /// Says a submit signalling `value` names the resource.
        void nameFor(std::uint64_t value) const { mNamedUntil = std::max(mNamedUntil, value); }

        /// The last value a submit naming the resource signals, or nought where nothing has.
        std::uint64_t getNamedUntil() const { return mNamedUntil; }

        /// Whether every submit that names the resource has run.
        ///
        /// A stamp for the next submit is not a hazard: that submit has not been made, and a host
        /// write made before it is what it sees — which is how a placement writes a mesh's rows,
        /// hands their address out, and writes the next mesh's. The stamp keeps only the last
        /// value, so this says nothing about an older submit still reading; a resource handed to
        /// two submits in flight is what the two copies of every table a frame writes exist to
        /// prevent. True of a resource nothing has named.
        bool isIdle(const Timeline& timeline) const
        {
            return mNamedUntil >= timeline.getNext() || timeline.hasFinished(mNamedUntil);
        }

        /// Blocks until `isIdle`, where a submit naming the resource is still on the queue. `what`
        /// names the wait in the error a device that stops answering produces.
        void waitIdle(const Timeline& timeline, const char* what) const
        {
            if (!isIdle(timeline))
                timeline.waitFor(mNamedUntil, what);
        }

    private:
        mutable std::uint64_t mNamedUntil = 0;
    };
}
