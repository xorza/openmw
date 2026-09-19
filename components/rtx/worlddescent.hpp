#pragma once

#include <algorithm>

#include <osg/Group>
#include <osg/LOD>
#include <osg/Node>
#include <osg/NodeVisitor>
#include <osg/Sequence>
#include <osg/Switch>

#include "nodekind.hpp"

namespace Rtx
{
    /// The child of an LOD authored for the nearest eye: the one whose range starts least far.
    /// Found by the ranges and not taken as the first child, because `NifOsg` cuts overlapping
    /// ranges apart and a child can then stand in the list more than once. Read only as far as
    /// both lists reach, because the loader states a range per level of the record before the
    /// children arrive, and a record can state more levels than it has children.
    inline unsigned int nearestLevel(const osg::LOD& levels)
    {
        unsigned int nearest = 0;
        const unsigned int ranged = std::min(levels.getNumRanges(), levels.getNumChildren());
        for (unsigned int at = 1; at < ranged; ++at)
            if (levels.getMinRange(at) < levels.getMinRange(nearest))
                nearest = at;

        return nearest;
    }

    /// Hands every child of `node` to `visitor` in order, telling `enterChild` the index of each
    /// first. What `osg::NodeVisitor::traverse` does, with the index said out loud.
    template <class EnterChild>
    void forEachChild(osg::Node& node, osg::NodeVisitor& visitor, EnterChild enterChild)
    {
        osg::Group* children = node.asGroup();
        if (children == nullptr)
            return;

        for (unsigned int at = 0; at < children->getNumChildren(); ++at)
        {
            enterChild(at);
            children->getChild(at)->accept(visitor);
        }
    }

    /// Descends into the children of `node` that are in the world, handing each to `visitor`. A
    /// switch is honoured, or `DayNightCallback` leaves the night lamp traced at noon and a
    /// harvested plant traced through the one it replaced. A sequence — `NiFltAnimationNode`, a
    /// fire or a forge — is honoured at the frame it stands on, or every frame of it is traced at
    /// once. An LOD is answered with its nearest level at every distance, because a range is a
    /// rasterizer's budget and a ray is owed the finest child, not the one a distance test picked
    /// and not every level standing at once. That is why both walks stay in
    /// `TRAVERSE_ALL_CHILDREN` and share this one rule.
    ///
    /// @param stepSequence run on a sequence before its frame is read. The mirror runs the
    ///        flipbook's clock here, because it lives in a traversal this renderer does not run;
    ///        a template's clock is nobody's to run.
    /// @param enterChild told which child of `node` is about to be handed to the visitor, before
    ///        it is. The mirror folds that index into the child's identity; a template walk
    ///        needs no identity and is told nothing.
    template <class StepSequence, class EnterChild>
    void descendInWorld(osg::Node& node, const NodeKind kind, osg::NodeVisitor& visitor, StepSequence stepSequence,
        EnterChild enterChild)
    {
        if (osg::Switch* branches = node.asSwitch())
        {
            for (unsigned int at = 0; at < branches->getNumChildren(); ++at)
                if (branches->getValue(at))
                {
                    enterChild(at);
                    branches->getChild(at)->accept(visitor);
                }

            return;
        }

        if (auto* frames = as<osg::Sequence>(kind, NodeKind::Sequence, node))
        {
            stepSequence(*frames);

            const int shown = frames->getValue();
            if (shown >= 0 && shown < static_cast<int>(frames->getNumChildren()))
            {
                enterChild(static_cast<unsigned int>(shown));
                frames->getChild(shown)->accept(visitor);
            }

            return;
        }

        if (auto* levels = as<osg::LOD>(kind, NodeKind::Lod, node))
        {
            if (levels->getNumChildren() > 0)
            {
                const unsigned int nearest = nearestLevel(*levels);
                enterChild(nearest);
                levels->getChild(nearest)->accept(visitor);
            }

            return;
        }

        forEachChild(node, visitor, enterChild);
    }
}
