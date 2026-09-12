#ifndef OPENMW_COMPONENTS_SCENEUTIL_LOCALTOWORLD_H
#define OPENMW_COMPONENTS_SCENEUTIL_LOCALTOWORLD_H

#include <osg/Matrix>

namespace osg
{
    class Node;
}

namespace SceneUtil
{
    /// The frame `node`'s own coordinates sit in, accumulated down the first parent at each level.
    ///
    /// **The answer `osg::Node::getWorldMatrices` gives for its first path, without the vectors.**
    /// That call collects every parental node path into a vector of vectors and returns a vector of
    /// matrices, and the weather's wrap-around operator asks it of every particle system on every
    /// frame — for a matrix that fits in a caller's own storage. A test asserts the two agree over
    /// every shape the chain can take: rotations, scales, several parents and an absolute reference
    /// frame.
    osg::Matrix localToWorldOf(const osg::Node& node);
}

#endif
