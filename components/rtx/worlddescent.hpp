#pragma once

#include <osg/Node>
#include <osg/NodeVisitor>
#include <osg/Sequence>
#include <osg/Switch>

#include "nodekind.hpp"

namespace Rtx
{
    /// Descends into the children of `node` that are in the world, handing each to `visitor`. A
    /// switch is honoured, or `DayNightCallback` leaves the night lamp traced at noon and a
    /// harvested plant traced through the one it replaced. A sequence — `NiFltAnimationNode`, a
    /// fire or a forge — is honoured at the frame it stands on, or every frame of it is traced at
    /// once. An LOD is not, because a ray is owed the finest child and not the one a distance test
    /// picked. That is why both walks stay in `TRAVERSE_ALL_CHILDREN` and share this one rule.
    ///
    /// @param stepSequence run on a sequence before its frame is read. The mirror runs the
    ///        flipbook's clock here, because it lives in a traversal this renderer does not run;
    ///        a template's clock is nobody's to run.
    template <class StepSequence>
    void descendInWorld(osg::Node& node, const NodeKind kind, osg::NodeVisitor& visitor, StepSequence stepSequence)
    {
        if (osg::Switch* branches = node.asSwitch())
        {
            for (unsigned int at = 0; at < branches->getNumChildren(); ++at)
                if (branches->getValue(at))
                    branches->getChild(at)->accept(visitor);

            return;
        }

        if (auto* frames = as<osg::Sequence>(kind, NodeKind::Sequence, node))
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
