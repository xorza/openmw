#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

#include <osg/Vec3f>
#include <osg/Vec4i>
#include <osg/ref_ptr>

#include "sceneextractor.hpp"

namespace Terrain
{
    class View;
    class World;
}

namespace Rtx
{

    /// The terrain's chunks, for a renderer that walks the world rather than culling it.
    ///
    /// **`Terrain::QuadTreeWorld` keeps its chunks out of the scene graph.** It resolves them inside
    /// a cull, against a `ViewData` keyed on the camera doing the culling, and parents them to
    /// nothing — so with `distant terrain` on, a mirror walking the graph finds no ground, no paged
    /// objects and no grass. `Terrain::World::collect` is the way to ask instead of walk, and this is
    /// what holds the view it needs.
    ///
    /// **A view of its own, and one view.** Two would be two sets of chunks at two levels of detail
    /// for one frame, which is what the reflection and the primary ray must not disagree about.
    ///
    /// **And a second view on a thread, which is what keeps `collect` cheap.** `collect` builds
    /// whatever it asks for that is not already built — a terrain chunk, and the merge of every
    /// static standing on it — on the frame that asks. `CellPreloader` warms the *camera's* view and
    /// only for a destination, so nothing warms this one: measured on a route in the game, the
    /// paging under `collect` was the largest cost of the main thread's frame after the walk itself.
    /// The thread here asks for the same square a little ahead of the eye, so the chunks the next
    /// frames want are built before they are asked for.
    class TerrainResidency final : public Residency
    {
    public:
        TerrainResidency();

        /// Stops the warming thread. A preload in flight is cut short rather than waited out.
        ~TerrainResidency() override;

        TerrainResidency(const TerrainResidency&) = delete;
        TerrainResidency& operator=(const TerrainResidency&) = delete;

        /// Which world to ask. Changing worldspaces makes a new one, and a view belongs to the world
        /// that handed it out, so the view goes with it.
        void follow(Terrain::World* terrain);

        /// Where the detail is chosen from — the eye, which is what a cull would have used.
        void setViewPoint(const osg::Vec3f& viewPoint) { mViewPoint = viewPoint; }

        void collect(osg::NodeVisitor& visitor) override;

    private:
        /// How many view points ahead the warming thread aims.
        ///
        /// **Warming where the eye stands buys nothing.** The chunk set changes when the eye crosses
        /// a level-of-detail boundary, and a thread warming the point `collect` just used has never
        /// asked for the set on the far side of it — so the frame that crosses still builds. Aiming
        /// ahead by the distance the eye covered over the last several frames is what puts the build
        /// before the crossing rather than on it.
        ///
        /// **Thirty and not sixty, measured.** Twice the lead warms a square centred further from
        /// the eye, so it spends the thread on chunks the near levels do not want yet: on the island
        /// route it moved the median frame from 7.1–7.9 ms to 8.5–9.2 and left the p99 and the worst
        /// frame where they were.
        static constexpr float sLeadSteps = 30.0f;

        /// How far ahead that aim may reach, whatever the eye did. A door or a fast travel moves the
        /// eye a worldspace at a time, and extrapolating that would warm a square nobody is going to
        /// look at.
        static constexpr float sLeadLimit = 8192.0f;

        /// Hands the thread the square to warm next, and where to centre it.
        void ask();

        void warm(std::stop_token stop);

        Terrain::World* mTerrain = nullptr;

        /// Null for a world that parents its chunks, which is what `createView` answers there. That
        /// world needs nothing from this, and `collect` says so by doing nothing.
        osg::ref_ptr<Terrain::View> mView;

        osg::Vec3f mViewPoint;

        /// Where the eye was and what the square was when the thread was last asked. The step
        /// between two of these is a heading and a speed without anything having to be told either,
        /// and a step of nothing is an ask worth skipping.
        osg::Vec3f mLastAsked;
        osg::Vec4i mLastGrid;
        bool mAskedOnce = false;

        std::mutex mMutex;
        std::condition_variable_any mWake;

        /// What the thread is to warm, written under the lock and read once it wakes. A frame that
        /// asks while it is busy overwrites this rather than queueing, so it always warms the
        /// newest place and never a backlog of stale ones.
        osg::Vec3f mWantedPoint;
        osg::Vec4i mWantedGrid;
        bool mWanted = false;

        /// **`Terrain::World::preload` is thread safe except into one view from two threads**, so
        /// the thread fills this one and `collect` fills `mView`.
        osg::ref_ptr<Terrain::View> mWarmView;

        /// Held by whichever of the two is inside `QuadTreeWorld::loadRenderingNode`.
        ///
        /// **The chunk caches test and then fill without holding anything.**
        /// `Terrain::ChunkManager::getChunk` and `Terrain::ObjectPaging::getChunk` each read their
        /// cache, build on a miss and write the result back, so two threads missing one key both
        /// build it and the later write wins — and the frame keeps whichever node its own call
        /// returned. What that costs a rasterizer is a chunk built twice; what it costs a mirror is
        /// a different drawable, which is a different mesh, a different material and a different
        /// instance. Measured over five runs of `bench` at Seyda Neen's shore, it moved the placed
        /// instance count between 15,959 and 15,964 and the mesh count by up to a hundred, and
        /// holding the two apart made all five agree exactly.
        ///
        /// **`ChunkManager` makes it a picture and not only a count**: a chunk with no cache entry
        /// of its own takes its passes and its composite map from whatever chunk of the same centre
        /// and level the cache happens to hold, so which thread built first decides what the ground
        /// is drawn with.
        std::mutex mBuilding;

        /// Stops a preload in flight, so the thread lets go of `mBuilding` within one chunk.
        ///
        /// **Two askers and one meaning.** A world going away needs the walk of a quad tree that is
        /// about to be destroyed cut short, and a frame taking the builder for itself needs the same
        /// thing. `preload` reads it between chunks either way, so the frame waits for one chunk
        /// rather than for the square the thread was given.
        std::atomic<bool> mYield{ false };

        /// **Last, so it is joined before anything it touches is destroyed.**
        std::jthread mWorker;
    };

}
