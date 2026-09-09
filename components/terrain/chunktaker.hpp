#ifndef COMPONENTS_TERRAIN_CHUNKTAKER_H
#define COMPONENTS_TERRAIN_CHUNKTAKER_H

#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4i>

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

    /// Where a collect looks from, and what of the world it may see.
    ///
    /// **Everything a collect would otherwise read off the world, captured by the thread that is
    /// allowed to read it.** `Terrain::World` is written by the game's own thread — `setActiveGrid`
    /// where a cell grid changes, `enable` where the player goes indoors — so a caller on any other
    /// thread has to be handed them rather than look. What is left for a collect to read of the
    /// world is settled at construction or guarded by a mutex of its own.
    struct Vantage
    {
        osg::Vec3f mViewPoint;

        /// The square the simulation holds. It decides a chunk's level of detail and whether it
        /// stands inside the active grid, which is part of what names its contents.
        osg::Vec4i mGrid;

        /// Whether the terrain is on the graph at all. `QuadTreeWorld::enable` is what takes it
        /// off, and a caller that may not see the terrain root may not see its chunks either.
        bool mEnabled = false;
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
