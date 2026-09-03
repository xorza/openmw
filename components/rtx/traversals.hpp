#pragma once

namespace Rtx
{
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
