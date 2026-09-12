#pragma once

#include <osg/Transform>
#include <osg/Viewport>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include "nodekind.hpp"

namespace Rtx
{
    /// A cull traversal that culls nothing and draws nothing, for the one thing left that answers
    /// only to an `osgUtil::CullVisitor`: `SceneUtil::RigGeometry` and `SceneUtil::MorphGeometry`
    /// skin inside `accept` by casting the visitor to one, and `OffscreenTrace::pick` reads that
    /// posed copy once per click. Not for a whole graph: `Terrain::TerrainDrawable::cull` puts the
    /// chunk in a render bin and never applies it. A real `CullVisitor`, because those casts are
    /// unchecked. Whoever uses it owes it a frame stamp — `SceneUtil::FrameTimeSource` reads the
    /// simulation time off it unchecked — and a traversal number a skeleton has not seen.
    class PoseCull : public osgUtil::CullVisitor
    {
    public:
        PoseCull()
        {
            setCullingMode(osg::CullSettings::NO_CULLING);
            setStateGraph(new osgUtil::StateGraph);

            // Nothing will ever be drawn out of it, but `accept` pushes the drawable's own state set
            // on the way past, and a state set naming a render bin sends the visitor to
            // `_currentRenderBin` — which a good deal of Morrowind's content names, the error marker
            // a missing model resolves to among them.
            setRenderStage(new osgUtil::RenderStage);

            // `CullStack` reads the back of each of these without checking, so they are pushed once
            // and never popped: an empty stack is not a permissive one, it is a crash.
            pushViewport(new osg::Viewport(0, 0, 1, 1));
            pushProjectionMatrix(new osg::RefMatrix);
            pushModelViewMatrix(new osg::RefMatrix, osg::Transform::ABSOLUTE_RF);
        }

        /// The pose is read off the drawable afterwards, so there is nothing to do with it here.
        void apply(osg::Drawable&) override {}

        /// Everything but a particle simulation, which a real cull visitor would otherwise run:
        /// `osgParticle` keeps a once-per-frame guard and a `_t0` per processor, so two visitors
        /// on two clocks difference a `_t0` from one against a time from the other.
        /// `SceneExtractor` owns the one emitter clock in this renderer.
        void apply(osg::Node& node) override
        {
            const NodeKind kind = mKinds.of(node);
            if (kind == NodeKind::ParticleProcessor || kind == NodeKind::ParticleUpdater)
                return;

            osgUtil::CullVisitor::apply(node);
        }

    private:
        NodeKinds mKinds;
    };
}
