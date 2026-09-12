#pragma once

#include <cassert>
#include <cstdint>

#include "extractionstats.hpp"

namespace Rtx
{
    /// What one walk is: which sweep stamps it, and where its counts go. Borrowed `const` by every
    /// resolver, so a pass is one state rather than four copies free to fall behind each other,
    /// and only the extractor moves a pass on.
    struct MirrorPass
    {
        std::uint64_t mEpoch = 0;

        /// Null between walks. A walk is not re-entrant — the extractor's anchor is a member set
        /// per walk — so there is one of these at a time, and `getStats` is what says so.
        ExtractionStats* mStats = nullptr;

        ExtractionStats& getStats() const
        {
            assert(mStats != nullptr && "a resolver reached outside a walk");
            return *mStats;
        }
    };

    /// The numbers mirror walks run at, and the rule that they only ever go up: a state-set
    /// controller, an `osg::Sequence` and both deforming drawables refuse to run for a traversal
    /// number they already saw. The world's walk and a traced view's must not be two sequences,
    /// because a subtree reached by both would be frozen for whichever got there second. Not the
    /// frame number, which says which of a `SceneUtil::LightSource`'s two buffers update wrote: a
    /// doll redrawn twice in one frame needs two traversal numbers and one light buffer.
    class Traversals
    {
    public:
        /// The next number, greater than every number handed out before it.
        unsigned int next() { return ++mLast; }

    private:
        /// From one and not from zero. Everything OSG poses starts at a traversal number of
        /// zero, so a first walk saying zero is a walk that poses nothing.
        unsigned int mLast = 0;
    };
}
