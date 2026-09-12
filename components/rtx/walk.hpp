#pragma once

#include <cassert>
#include <cstdint>

#include "extractionstats.hpp"

namespace Rtx
{
    /// What one walk is: which sweep stamps it, and where its counts go.
    ///
    /// **Borrowed by every resolver**, so a pass is one state rather than four copies free to fall
    /// behind each other. The epoch was already shared for that reason, and the counts are the
    /// other half of the same fact — what this pass met, as against what the last one did.
    ///
    /// **Borrowed `const`, because only the extractor moves a pass on.** A resolver reads the epoch
    /// and writes the counts through it, and neither advancing the sweep nor repointing the counts
    /// is a resolver's to do.
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

    /// The numbers mirror walks run at, and the rule that they only ever go up.
    ///
    /// **A state-set controller, an `osg::Sequence`, and — under the pick's own cull — both deforming
    /// drawables refuse to run for a traversal number they have already seen.** So a walk's number
    /// is not a label but a claim: this frame is newer than the last.
    ///
    /// This fork has two things that walk — the world, once a frame, and a traced view whenever its
    /// subject changes — and they must not be two sequences. A subtree reached by both would be run
    /// by whichever got there first and frozen for the other, and nothing states that no subtree is
    /// shared: `NpcAnimation` merely happens to clone a `RigGeometry` per instance. One counter, and
    /// the hazard cannot arise.
    ///
    /// **Not the frame number**, which a walk also carries and which means something else — which of
    /// a `SceneUtil::LightSource`'s two buffers update has just written. A doll redrawn twice in one
    /// frame needs two traversal numbers and one light buffer.
    class Traversals
    {
    public:
        /// The next number, greater than every number handed out before it.
        unsigned int next() { return ++mLast; }

    private:
        /// **From one and not from zero.** Everything OSG poses starts at a traversal number of
        /// zero, so a first walk saying zero is a walk that poses nothing.
        unsigned int mLast = 0;
    };
}
