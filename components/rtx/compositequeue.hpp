#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <osg/Image>
#include <osg/ref_ptr>

#include "run.hpp"
#include "scenedesc.hpp"
#include "shadingcache.hpp"
#include "terraincomposite.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    /// How many finished composites one `collect` may move into the scene.
    ///
    /// **A bound on what an arrival frame pays, in the unit that costs.** Every composite taken is
    /// a texture arriving — an image created, a megabyte and a half staged, a descriptor written —
    /// and a long frame can find a dozen finished behind it. Two a frame is under a millisecond;
    /// the rest wait a frame each, shading from their stacks as they did while they baked.
    inline constexpr std::size_t sCompositesPerFrame = 2;

    /// Every distant chunk waiting for its ground to be flattened, and the thread that flattens them.
    ///
    /// **The bake happens on no frame at all.** One costs 28.5 ms and a cell boundary wants
    /// several; sliced sixteen rows a frame it was a millisecond or two on every frame for twenty
    /// seconds after a load, and the row that finished one was a spike on top of that. A thread of
    /// this queue's own takes each stack whole, and what the frame does is hand a stack over and
    /// take the bytes back — a copy of a few hundred floats each way.
    ///
    /// **Nothing is wrong while it waits.** A chunk asks by setting `Material::mFlatten` and its
    /// `mDiffuse` stays unset, which is the branch the shader already takes for every near chunk: it
    /// sums the layer stack at the hit. So the picture is right from the first frame and what the
    /// bake buys is the cost of that hit, not the sight of the ground.
    ///
    /// **A finished composite is held only until it is uploaded.** The bytes are a megabyte and a
    /// half apiece, and a region's worth is the same fifty megabytes the texture array already holds
    /// — keeping a second copy of that on the host would be paying twice for one picture.
    class CompositeQueue
    {
    public:
        CompositeQueue() = default;

        /// Stops the baker. A bake in flight finishes first; what is queued behind it does not.
        ~CompositeQueue() = default;

        CompositeQueue(const CompositeQueue&) = delete;
        CompositeQueue& operator=(const CompositeQueue&) = delete;

        /// Whether a hand-over waits for the bakes it queued before it takes any.
        ///
        /// **Which frame a composite lands on is otherwise the baker thread's answer, not the
        /// schedule's.** A chunk arrives, a bake is queued, and it comes back whenever a thread
        /// finishes it — so two runs of one build take it on different frames and draw different
        /// pictures from the crossing onwards. That is a run that cannot be compared with itself,
        /// which is the same reason `Rtx::FrameOptions::mSinceLast` exists.
        ///
        /// **The per-frame bound is kept.** Waiting is not collecting: a settled run still takes
        /// `sCompositesPerFrame` and no more, so the arrival pattern is the one the streaming path
        /// really has and only the thread's timing is gone. What it costs is a stall at the
        /// crossing that queued the bakes, which is a trade a measured run can make and a game
        /// cannot.
        ///
        /// **This is the only thread a settled run waits on, and the terrain is not one.** The quad
        /// tree is the obvious suspect and the wrong one: `Terrain::QuadTreeWorld::collect` resolves
        /// its view and loads every entry it names, in the calling thread.
        void setSettled(bool settled) { mSettled = settled; }

        /// Hands the baker what this walk marked, then moves what is finished into the scene.
        ///
        /// **Before anything reads what arrived, because a composite coming back is an arrival.** A
        /// composite taken here took a texture slot on the way, so the upload that follows carries
        /// it without knowing it was ever waiting.
        ///
        /// @return how many landed, which is what makes a frame that only finished a bake an
        ///         arrival for everything downstream.
        std::size_t advance(SceneDesc& scene, Resource::ImageManager& images);

        /// The finished composite in `slot`, or null where nothing here baked one.
        const TerrainComposite* find(Index slot) const;

        /// Lets go of everything `collect` took. **After the upload and not before**: what is held
        /// between those two calls is the only copy of the bytes a backend has to read.
        void releaseFinished() { mFinished.clear(); }

    private:
        /// Hands the baker every chunk the walk wrote that wants flattening and is not already
        /// handed over.
        ///
        /// **Off the rows the scene says it wrote**, which is the only place a chunk wanting a
        /// composite can appear; the table itself is never scanned. Everything the bake reads — the
        /// images, the weights, the transforms — is taken here, so the thread reads nothing the next
        /// walk can change.
        void gather(const SceneTables& scene, Resource::ImageManager& images);

        /// Waits until nothing handed over is still baking.
        void finish();

        /// Moves at most `limit` finished composites into the scene, oldest first.
        ///
        /// A composite taken takes a texture slot and goes onto the material that asked. One whose
        /// chunk left the world while it baked is dropped instead.
        ///
        /// @return how many were taken.
        std::size_t collect(SceneDesc& scene, std::size_t limit);

        bool mSettled = false;

        /// Which chunk asked: the material's slot and where its layers sat when it did.
        ///
        /// What the frame side remembers of everything handed over and not yet collected, without
        /// taking the lock — and what a bake is matched back to, so a slot another chunk took over
        /// in the meantime is not handed the first one's ground.
        struct Asked
        {
            Index mMaterial = sNoIndex;
            Run mLayers;

            bool operator==(const Asked& other) const = default;
        };

        /// One chunk's stack as the baker reads it.
        ///
        /// **Copied, every part of it.** A bake outlives the walk that asked for it, so the weights
        /// and the layers are taken by value and the images by reference count; the scene may free
        /// the chunk's runs and hand them to another while this is being read, and nothing here
        /// notices. The layers as the scene had them are what `collect` compares against to know the
        /// same chunk still stands.
        struct Request
        {
            Asked mAsked;

            std::vector<MaterialLayer> mLayers;

            /// Parallel to `mLayers`, null where the layer's file could not be opened.
            std::vector<osg::ref_ptr<const osg::Image>> mImages;

            /// Every layer's weights end to end, and where each layer's run sits in them — a count
            /// of nought for a layer that covers the chunk.
            std::vector<float> mMasks;
            std::vector<Run> mMaskRuns;

            /// Empties the four without giving their room back, so a request taken off `mSpare`
            /// starts empty and keeps the buffers the last chunk grew.
            ///
            /// The images go: a reference held in a spare buffer keeps a picture alive for a chunk
            /// that has already been baked and collected.
            void reuse()
            {
                mLayers.clear();
                mImages.clear();
                mMasks.clear();
                mMaskRuns.clear();
            }
        };

        /// What came back: the request, and the composite — or none, where every layer was
        /// unreadable and the chunk keeps its stack.
        struct Baked
        {
            Request mRequest;
            std::optional<TerrainComposite> mComposite;
        };

        /// The baker's loop: one request at a time, until asked to stop.
        void work(std::stop_token stop);

        /// Describes, estimates and flattens one stack. On the baker's thread.
        Baked bake(Request&& request);

        /// Guards `mPending`, `mDone` and `mBaking` — everything the two threads share.
        std::mutex mMutex;

        /// Woken by a request arriving, or by the stop.
        std::condition_variable_any mWake;

        /// Woken by a bake finishing, which is what `finish` waits for.
        std::condition_variable mBaked;

        /// Oldest first, so the baker finishes chunks in the order they arrived.
        std::deque<Request> mPending;
        std::deque<Baked> mDone;
        std::size_t mBaking = 0;

        /// Everything handed over and not yet collected, which is what `gather` checks against.
        std::vector<Asked> mAsked;

        /// Collected this frame, by the slot they were given. Emptied by `releaseFinished`.
        std::unordered_map<Index, TerrainComposite> mFinished;

        /// Refilled per collect rather than built afresh: the frame a composite lands on is not
        /// one to spend an allocation on.
        std::vector<Baked> mTaken;

        /// Requests that have been through the queue and are waiting to carry another chunk.
        ///
        /// **A chunk costs four vectors, and a crossing gathers dozens.** Without this, `gather`
        /// builds them on the frame the chunk arrives and `collect` frees them on the frame its
        /// ground comes back — a route across the island paying for every one of them twice over.
        /// What comes back here keeps the room it grew.
        ///
        /// The game thread's own, at both ends: `gather` takes from it and `collect` returns to it,
        /// and the baker sees neither.
        ///
        /// **Everything here has been through `reuse`**, which is what putting one back means — so
        /// one taken off is empty and holds no image, and `gather` fills it without clearing it
        /// first.
        std::vector<Request> mSpare;

        std::string mKey;

        /// **The baker's thread and no other**, because that is where a stack is described and the
        /// cache is not guarded. The estimate is node-based, which is what lets the stack span it.
        ShadingCache mPainted;

        /// What `bake` reads a stack into, and what a bake works in.
        ///
        /// **The baker's thread too, and `bake`'s alone.** Nothing else here reads them, which is
        /// what lets `mStackScratch` go on spanning `mLevelScratch` between chunks: the next bake
        /// clears both before it fills either.
        ///
        /// Held rather than made, for the reason `mSpare` is: a crossing bakes dozens of chunks, and
        /// a working set made per chunk is the same megabytes taken and given back dozens of times.
        std::vector<MipLevel> mLevelScratch;
        std::vector<CompositeLayer> mStackScratch;
        CompositeScratch mScratch;

        /// **Last, so it is joined first.** A member declared above it would be destroyed while
        /// the baker was still reading it; the stop the join begins with is what wakes the wait.
        /// Started by the first chunk that asks rather than with the queue: a world that never
        /// reaches distant ground never pays for a thread.
        std::jthread mWorker;
    };
}
