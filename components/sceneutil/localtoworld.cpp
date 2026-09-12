#include "localtoworld.hpp"

#include <osg/Node>
#include <osg/NodeVisitor>
#include <osg/Transform>

namespace SceneUtil
{
    namespace
    {
        void accumulateLocalToWorld(const osg::Node& node, osg::Matrix& into, osg::NodeVisitor& asking)
        {
            if (node.getNumParents() > 0)
                accumulateLocalToWorld(*node.getParent(0), into, asking);

            if (const osg::Transform* transform = node.asTransform())
                transform->computeLocalToWorldMatrix(into, &asking);
        }
    }

    osg::Matrix localToWorldOf(const osg::Node& node)
    {
        // **A plain visitor, and the caller's own would be the wrong one.** What this walk must not
        // say is that it is a cull: `MWRender::CameraRelativeTransform` casts a visitor of that type
        // to `osgUtil::CullVisitor` and reads a view point off it, and the ray tracer's mirror calls
        // itself a cull visitor for as long as it steps a particle system. A default one is a
        // `NODE_VISITOR`, which every transform here answers with the transform alone.
        //
        // On the stack and not kept: it holds nothing and allocates nothing, and one is made per
        // step of a particle system rather than per particle.
        osg::NodeVisitor asking;

        osg::Matrix matrix;
        accumulateLocalToWorld(node, matrix, asking);
        return matrix;
    }
}
