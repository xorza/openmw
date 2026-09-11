#pragma once

#include <osg/Node>

#include <components/esm/refid.hpp>

namespace Terrain
{
    class ObjectStorage;
    class Storage;
}

namespace Rtx
{
    class ContentSource;

    /// Where the world's cells are read from: the content, and which worldspace of it.
    ///
    /// **One value, because it is one question.** These are exactly `CellReader`'s arguments, and a
    /// change to any of them is a reader that has to be built again — so they are compared as one
    /// rather than field by field at the call that asks. The frame states it once, inside
    /// `WorldAround`, and every residency reads the same one.
    struct CellWorld
    {
        /// What the content files say stands where, or null for a world with none.
        const Terrain::ObjectStorage* mStorage = nullptr;

        /// The land the heights and the blend maps are read off.
        Terrain::Storage* mGround = nullptr;

        /// The loader the models and the images come out of.
        ContentSource* mContent = nullptr;

        ESM::RefId mWorldspace;

        /// Which nodes a walk of a template may descend into — the frame walk's own.
        osg::Node::NodeMask mMask = ~0u;

        /// Whether there is enough here to read anything at all.
        bool isReadable() const { return mStorage != nullptr && mGround != nullptr && mContent != nullptr; }

        bool operator==(const CellWorld& other) const = default;
    };
}
