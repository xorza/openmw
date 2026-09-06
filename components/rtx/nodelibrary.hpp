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

    /// Whether `node` is exactly `type`, for the cast a library name cannot narrow.
    ///
    /// **The class where the library is every node in a cell.** `isFrom` is the gate to reach for,
    /// because one library rules out whole kinds at once; it says nothing where the library is
    /// `osg` itself, which every plain group in a cell belongs to. This asks the narrower question
    /// at the same price, and it is sound only for a type nothing derives from — a subclass answers
    /// with its own name.
    inline bool isExactly(const osg::Node& node, const char* type)
    {
        return std::strcmp(node.className(), type) == 0;
    }
}
