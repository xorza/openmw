#pragma once

#include <osgUtil/UpdateVisitor>

#include <components/sceneutil/lightmanager.hpp>

#include "nodelibrary.hpp"

namespace Rtx
{
    /// An update traversal that leaves a light where it stands.
    ///
    /// **Everything but a `SceneUtil::LightSource`, whose update callbacks are the rasterizer's.**
    /// `SceneUtil::CollectLightCallback` adds the light to the nearest `SceneUtil::LightManager`
    /// above it, which is the list a cull traversal binds into a state set — and it throws where
    /// there is no manager to add it to. This renderer reads the `LightSource` node itself, so that
    /// list was filled every frame for nobody, and a harness that stands a lamp under no manager at
    /// all could not pose the actor holding it.
    ///
    /// **The animation the same chain carries is read rather than run.** `SceneUtil::LightController`
    /// writes one frame's colours into the light; `Rtx::lightColour` asks the controller what the
    /// light does instead — its type, and the base colour it was built with — and works the flicker
    /// out from the simulation time, which is what makes a still comparable with another still.
    ///
    /// The node is a leaf, so stopping at it loses nothing under it.
    class PoseUpdate : public osgUtil::UpdateVisitor
    {
    public:
        void apply(osg::Node& node) override
        {
            if (isFrom(node, "SceneUtil") && dynamic_cast<SceneUtil::LightSource*>(&node) != nullptr)
                return;

            osgUtil::UpdateVisitor::apply(node);
        }
    };
}
