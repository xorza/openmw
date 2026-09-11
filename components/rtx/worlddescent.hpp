#pragma once

#include <osg/Node>
#include <osg/NodeVisitor>
#include <osg/Sequence>
#include <osg/Switch>

#include "nodelibrary.hpp"

namespace Rtx
{
    /// Descends into the children of `node` that are in the world, handing each to `visitor`.
    ///
    /// **Three node types in this tree choose among their children, and they get three answers.**
    /// A switch is honoured. A sequence is honoured, and `stepSequence` decides whether its clock
    /// runs first. An LOD is not honoured at all, because a ray is owed the finest child a node has
    /// rather than the one a distance test picked for an eye. That is the whole of the decision, and
    /// it is why both walks stay in `TRAVERSE_ALL_CHILDREN`: the one mode that would answer the
    /// first two also answers the third, and it answers it wrongly.
    ///
    /// **Both walks in this renderer need all three**, and they differ only in that step — which is
    /// what made this one rule written twice, in two files, with a fourth line between them.
    ///
    /// **`osg::Switch`**: its `traverse` visits every child under `TRAVERSE_ALL_CHILDREN`, so a
    /// branch that is switched off is mirrored anyway. `MWRender`'s `DayNightCallback` leaves the
    /// night lamp traced at noon and the day mesh traced at midnight, both at once, and a harvested
    /// plant is traced through the unharvested one it replaced. This is geometry and not only light.
    ///
    /// **`osg::Sequence`**: `NifOsg` builds one for every `NiFltAnimationNode`, which is Morrowind's
    /// flipbook — a fire, a forge, a lava flow. Under `TRAVERSE_ALL_CHILDREN` every frame of it is
    /// traced at once and in the same place, and its clock never moves. Whichever frame it stands on
    /// is walked here, and the clock is `stepSequence`'s to run.
    ///
    /// @param stepSequence run on a sequence before its frame is read. The mirror runs the
    ///        flipbook's clock here, because that clock lives in a traversal this renderer does not
    ///        run; a template's clock is nobody's to run, so that walk passes one that does nothing.
    template <class StepSequence>
    void descendInWorld(osg::Node& node, osg::NodeVisitor& visitor, StepSequence stepSequence)
    {
        if (osg::Switch* branches = node.asSwitch())
        {
            for (unsigned int at = 0; at < branches->getNumChildren(); ++at)
                if (branches->getValue(at))
                    branches->getChild(at)->accept(visitor);

            return;
        }

        // Cast the group and not the node: these walks reach far more drawables than groups, and
        // only a group can be a sequence. And the class and not the library, because the library
        // here is `osg` — every plain group in a cell.
        if (auto* frames = isExactly(node, "Sequence") ? dynamic_cast<osg::Sequence*>(node.asGroup()) : nullptr)
        {
            stepSequence(*frames);

            const int shown = frames->getValue();
            if (shown >= 0 && shown < static_cast<int>(frames->getNumChildren()))
                frames->getChild(shown)->accept(visitor);

            return;
        }

        visitor.traverse(node);
    }
}
