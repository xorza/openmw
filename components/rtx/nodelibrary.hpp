#pragma once

#include <cstring>

#include <osg/Node>

namespace Rtx
{
    /// Whether `node` comes from `library`, which is the one question cheap enough to ask of every
    /// node before a `dynamic_cast` that nearly all of them fail.
    ///
    /// A node from `osg` or `NifOsg` — which is nearly every node in a cell — answers this in a byte
    /// where a failed cast walks the class hierarchy to say the same thing. Every traversal in this
    /// renderer runs over the whole graph, so the gate is what keeps a per-node question off the
    /// frame's cost.
    inline bool isFrom(const osg::Node& node, const char* library)
    {
        return std::strcmp(node.libraryName(), library) == 0;
    }
}
