#pragma once

#include <cstdint>

#include "device.hpp"
#include "timeline.hpp"

namespace Rtx
{
    /// The submits that name a resource, as the values they signal on the queue's timeline — what
    /// a host write of the resource waits for and asserts against, and what a destructor asserts,
    /// because a host write over a submit still reading is the one hazard the layers cannot see.
    /// Held by a buffer, an image and a structure, and by a descriptor set, which is bound by
    /// handle and has no buffer to carry one.
    ///
    /// **Stamped at the hand-out and nowhere else.** An address, a descriptor, a barrier, a copy's
    /// ends and a set bound each name their resource for the next submit as they hand it out, so
    /// the stamp is exact for every reader on the queue whatever carried it — a frame's trace, a
    /// placement, a deferred picture riding the interface's own submit. A count kept beside it in
    /// frames was exact for the first of those and wrong for the rest.
    ///
    /// **Two words, so a submit in flight is seen behind a naming for the next.** The newest
    /// naming alone read idle for a resource named by a submit in flight and again for the
    /// pending one. What decides idleness is the newest naming for a submit already made: the
    /// queue finishes in order, so once that has run every older naming has too. That is the
    /// newest naming where it is made, and the one before it where the newest is pending — so
    /// the two newest distinct namings are the whole of what has to be kept.
    ///
    /// Mutable throughout, because naming is not a change to the bytes and every caller that names
    /// holds the resource const.
    class ReadStamp
    {
    public:
        /// Says a submit signalling `value` names the resource. `value` is `Timeline::getNext`
        /// at every caller, so values never fall.
        void nameFor(const std::uint64_t value) const
        {
            if (value == mNewest)
                return;

            mPrevious = mNewest;
            mNewest = value;
        }

        /// The last value a submit naming the resource signals, or nought where nothing has.
        std::uint64_t getNamedUntil() const { return mNewest; }

        /// Whether the host has waited past every submit that names the resource.
        ///
        /// A stamp for the next submit is not a hazard: that submit has not been made, and a host
        /// write made before it is what it sees — which is how a placement writes a mesh's rows,
        /// hands their address out, and writes the next mesh's. True of a resource nothing has
        /// named.
        bool isIdle(const Device& device) const { return device.getTimeline().hasFinished(newestMade(device)); }

        /// Blocks until `isIdle`, where a submit naming the resource is still on the queue. `what`
        /// names the wait in the error a device that stops answering produces.
        void waitIdle(const Device& device, const char* what) const
        {
            if (!isIdle(device))
                device.waitFor(newestMade(device), what);
        }

    private:
        /// The newest naming for a submit already made: the newest where it is, and the one before
        /// it where the newest is the pending submit.
        std::uint64_t newestMade(const Device& device) const
        {
            return mNewest < device.getTimeline().getNext() ? mNewest : mPrevious;
        }

        /// The two newest distinct namings. Nought is a naming nothing has to wait for.
        mutable std::uint64_t mPrevious = 0;
        mutable std::uint64_t mNewest = 0;
    };
}
