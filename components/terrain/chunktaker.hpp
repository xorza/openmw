#ifndef COMPONENTS_TERRAIN_CHUNKTAKER_H
#define COMPONENTS_TERRAIN_CHUNKTAKER_H

#include <osg/Vec2f>

namespace osg
{
    class Node;
}

namespace Terrain
{
    /// What a chunk is: where it sits, how wide it is, whether it stands in the square the
    /// simulation holds, and the neighbours' levels of detail it was built against.
    ///
    /// **Everything `ChunkManager::getChunk` is asked, less the view point.** Those are what decide
    /// a chunk's contents, so together they name the contents rather than the node they arrive on —
    /// which is a different thing, and a thing nothing should keep. The view point is left out
    /// deliberately: `Terrain::ObjectPaging` reads it and does not key its cache on it, so a chunk
    /// already differs by it whatever anyone here does.
    ///
    /// **`mLod` is not here** because it is `size` and `minSize` and nothing else, so it would name
    /// nothing the other fields do not.
    struct ChunkName
    {
        osg::Vec2f mCentre;
        float mSize = 0.0f;
        unsigned int mLodFlags = 0;
        bool mActiveGrid = false;
    };

    /// What a world hands its chunks to, when nothing in the graph parents them.
    ///
    /// **A name and a node, together and in one call.** A caller that keeps an identity for what it
    /// finds needs the chunk's own name rather than the address of the transform it arrives on:
    /// `loadRenderingNode` makes a fresh transform whenever an entry has none and hands it whatever
    /// the chunk cache already held, so the same ground comes back on a new node at whatever address
    /// was free. Handing the two over together is what keeps the taker from having to remember one
    /// while it waits for the other.
    class ChunkTaker
    {
    public:
        virtual ~ChunkTaker() = default;

        /// Takes one chunk: what it is, and the node it arrived on.
        virtual void takeChunk(const ChunkName& name, osg::Node& node) = 0;
    };
}

#endif
