#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

#include <osg/Image>
#include <osg/ref_ptr>

#include "run.hpp"
#include "scenedesc.hpp"
#include "shadingcache.hpp"
#include "terraincomposite.hpp"
#include "worker.hpp"

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

    /// How many frames a stack may take to flatten before a frame waits for it.
    ///
    /// **A settled frame waits for the ground it collects, so what it must not collect is ground it
    /// asked for a moment ago.** A walk adopts one cell a frame and a cell hands over one stack, so
    /// without this the frame that queues a stack is the frame that waits out its whole bake.
    ///
    /// **Sized on the fastest run and not the target one**, because a frame of slack is worth less
    /// wall time the faster a machine draws — and a fast machine is the one a stall shows on.
    inline constexpr std::size_t sBakeFrames = 16;

    /// Every distant chunk waiting for its ground to be flattened, and the threads that flatten them.
    ///
    /// **The bake happens on no frame at all.** One costs 38 ms and a ring fill wants eighty-five;
    /// sliced sixteen rows a frame it was a millisecond or two on every frame for twenty seconds
    /// after a load, and the row that finished one was a spike on top of that. Threads of this
    /// queue's own take each stack whole, and what the frame does is hand a stack over and take the
    /// bytes back — a copy of a few hundred floats each way.
    ///
    /// **Several threads, because one is slower than the ground arrives.** A fill of eighty-five is
    /// 3.2 s of summing behind a frame that collects two of them, and a queue that deep is one every
    /// third chunk leaves the world before its ground comes back. What the count is measured against
    /// is in `compositequeue.cpp`.
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
        /// **A frame waits for what it collects, never for what it queued.** `sCompositesPerFrame`
        /// is what a frame takes, so two is what it waits for and the rest go on baking behind it.
        /// Draining the queue instead put every bake of a run onto the frame thread and gave the
        /// bakers nothing to do: measured on `one-cell-walk`, 9.9 s of bake over an 18.6 s run was
        /// waited for to the millisecond, and one ring fill was a single 3.3 s frame. Waiting for
        /// the two collected costs 0.14 s over the same run.
        ///
        /// **The order is the queue's and not the bakers'.** Several threads finish out of order, so
        /// `collect` takes by the sequence a stack was handed over in and never by what came back
        /// first. That is what makes the schedule the answer; the game keeps the same rule, because
        /// a composite that waits a frame for the one in front of it shades from its stack for one
        /// more frame and costs nothing else.
        ///
        /// **This is the only thread a settled run waits on, and the terrain is not one.** The quad
        /// tree is the obvious suspect and the wrong one: `Terrain::QuadTreeWorld::collect` resolves
        /// its view and loads every entry it names, in the calling thread.
        void setSettled(bool settled) { mSettled = settled; }

        /// Hands the bakers what this walk marked, then moves what is finished into the scene.
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
        /// Hands the bakers every chunk the walk wrote that wants flattening and is not already
        /// handed over.
        ///
        /// **Off the rows the scene says it wrote**, which is the only place a chunk wanting a
        /// composite can appear; the table itself is never scanned. Everything the bake reads — the
        /// images, the weights, the transforms — is taken here, so no baker reads anything the next
        /// walk can change.
        void gather(const SceneTables& scene, Resource::ImageManager& images);

        /// Waits until every stack `collect` is due to take has come back.
        void waitFor(std::size_t limit);

        /// How many of the sequences from `mNextTake` are old enough to collect, counted no further
        /// than `limit`.
        ///
        /// **The whole of what a settled frame takes.** It reads the frame count and the hand-over
        /// frames and nothing a baker touches, so what a frame collects is the schedule's answer
        /// and the wait is only how the frame is made to agree with it.
        std::size_t getDue(std::size_t limit) const;

        /// How many of the sequences from `mNextTake` have come back, counted no further than
        /// `limit`. Under `mMutex`.
        std::size_t getReady(std::size_t limit) const;

        /// Moves the composites that are due into the scene, in the order they were handed over,
        /// and at most `limit` of them.
        ///
        /// **It stops at the first sequence that is not due or has not come back**, so one the
        /// bakers finished early waits for the one in front of it. `setSettled` says why the order
        /// is the queue's and `sBakeFrames` why a stack is not due at once.
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

            /// Where this sits in the order the frame handed stacks over, which is the order
            /// `collect` takes them back in. Every sequence handed out reaches `mDone` exactly
            /// once — a chunk whose slot was taken over while it waited arrives with no composite
            /// rather than not arriving.
            std::uint64_t mSequence = 0;

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

        /// One thread and everything only it may touch.
        ///
        /// **Held apart per thread rather than guarded**, because a bake writes megabytes of
        /// working set and a lock around that would put the threads back in single file. The
        /// shading cache is per thread for the same reason, and the layers a region repeats cost
        /// each thread the first estimate of them.
        struct Baker
        {
            /// Describes, estimates and flattens one stack, on this baker's thread.
            ///
            /// **The baker's and not the queue's**, because everything it reads and writes besides
            /// the request is below: the four buffers and the cache. The queue owns the channel
            /// between the threads and nothing of the flattening.
            Baked bake(Request&& request);

            /// **This thread's and no other's**, because that is where a stack is described. The
            /// estimate is node-based, which is what lets the stack span it.
            ShadingCache mPainted;

            /// What `bake` reads a stack into, and what a bake works in.
            ///
            /// Held rather than made, for the reason `mSpare` is: a fill bakes dozens of chunks,
            /// and a working set made per chunk is the same megabytes taken and given back dozens
            /// of times. `mStackScratch` goes on spanning `mLevelScratch` between chunks, because
            /// the next bake clears both before it fills either.
            std::vector<MipLevel> mLevelScratch;
            std::vector<CompositeLayer> mStackScratch;
            CompositeScratch mScratch;

            /// **Last, so it is joined before anything above it is destroyed.**
            Worker mWorker;
        };

        /// Files `baked` under its sequence, so `mDone` reads in the order the stacks were handed
        /// over however the bakers finished them. Under `mMutex`.
        void file(Baked&& baked);

        /// Starts every baker, once. Called by the first chunk that asks, so a world that never
        /// reaches distant ground never pays for a thread.
        void startBakers();

        /// A baker's loop: one request at a time, until asked to stop.
        void work(Baker& baker, std::stop_token stop);

        /// Guards `mPending`, `mDone` and `mBaking` — everything the frame and the bakers share.
        std::mutex mMutex;

        /// Woken by a request arriving, or by the stop.
        std::condition_variable_any mWake;

        /// Woken by a bake finishing, which is what `waitFor` waits for.
        std::condition_variable mBaked;

        /// Oldest first, so the bakers take chunks in the order they arrived.
        std::deque<Request> mPending;

        /// By sequence and not by when a bake finished. See `file`.
        std::deque<Baked> mDone;
        std::size_t mBaking = 0;

        /// The next sequence to hand out, and the next to collect. **The frame thread's own**: one
        /// is written by `gather` and the other by `collect`, and no baker reads either.
        std::uint64_t mNextGiven = 0;
        std::uint64_t mNextTake = 0;

        /// How many frames have been handed over, which is what a stack's age is measured in.
        std::size_t mFrame = 0;

        /// The frame each sequence from `mNextTake` was handed over on, oldest first.
        ///
        /// **The frame thread's own too**, and one entry a sequence: `gather` appends where it hands
        /// a stack over and `collect` removes where it takes one back, so the front of this is
        /// always `mNextTake`'s.
        std::deque<std::size_t> mQueuedAt;

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

        /// **Last, so the threads are joined first.** A member declared above would be destroyed
        /// while a baker was still reading it; the stop the join begins with is what wakes the wait.
        /// Held by pointer so that a baker keeps its address, which its own thread captured.
        std::vector<std::unique_ptr<Baker>> mBakers;
    };
}
