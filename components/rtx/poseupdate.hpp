#pragma once

#include <osgUtil/UpdateVisitor>

#include <components/sceneutil/lightmanager.hpp>

#include "nodekind.hpp"

namespace Rtx
{
    /// An update traversal that leaves a light where it stands, because a
    /// `SceneUtil::LightSource`'s update callbacks are the rasterizer's:
    /// `SceneUtil::CollectLightCallback` throws where there is no `SceneUtil::LightManager` above
    /// it, and `SceneUtil::LightController`'s flicker is worked out by `Rtx::lightColour` from
    /// the simulation time instead, which is what makes a still comparable with another still.
    /// The node is a leaf, so stopping at it loses nothing under it.
    class PoseUpdate : public osgUtil::UpdateVisitor
    {
    public:
        void apply(osg::Node& node) override
        {
            if (mKinds.of(node) == NodeKind::LightSource)
                return;

            osgUtil::UpdateVisitor::apply(node);
        }

    private:
        NodeKinds mKinds;
    };
}
