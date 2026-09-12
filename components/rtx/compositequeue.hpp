#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <osg/Image>
#include <osg/ref_ptr>

#include "monitor.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"
#include "scratch.hpp"
#include "shadingmap.hpp"
#include "terraincomposite.hpp"
#include "texturedata.hpp"
#include "worker.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    /// How many finished composites one `collect` may move into the scene — a bound on what an
    /// arrival frame pays, because every composite taken is a megabyte and a half staged. Two a
    /// frame is under a millisecond; the rest wait a frame each, shading from their stacks.
    inline constexpr std::size_t sCompositesPerFrame = 2;

    /// How many frames a stack may take to flatten before a frame waits for it: a settled frame
    /// waits for the ground it collects, so without this the frame that queues a stack is the frame
    /// that waits out its whole bake. Sized on the fastest run, where a frame of slack is worth the
    /// least wall time.
    inline constexpr std::size_t sBakeFrames = 16;

    /// Every distant chunk waiting for its ground to be flattened, and the threads that flatten them.
    /// The bake happens on no frame at all, because one is tens of milliseconds and a ring fill
    /// wants dozens: threads of this queue's own take each stack whole, and the frame only hands a
    /// stack over and takes the bytes back. Several threads, because one is slower than the ground
    /// arrives (`bakerCount`). Nothing is wrong while it waits: a chunk that asked keeps `mDiffuse`
    /// unset and the shader sums its layer stack at the hit, so the bake buys the cost of that hit
    /// and not the sight of the ground. A finished composite is held only until it is uploaded,
    /// because a region's worth is fifty megabytes the texture array already holds.
    class CompositeQueue
    {
    public:
        CompositeQueue() = default;

        /// Stops the baker. A bake in flight finishes first; what is queued behind it does not.
        ~CompositeQueue() = default;

        /// Whether a frame waits for the bakes it is due to collect. Which frame a composite lands
        /// on is otherwise the baker thread's answer, so two runs of one build draw different
        /// pictures from the crossing onwards — the same reason `Rtx::FrameOptions::mSinceLast`
        /// exists. A frame waits for what it collects and never for what it queued, and takes by
        /// the sequence a stack was handed over in. This is the only thread a settled run waits on:
        /// the quad tree loads every entry it names in the calling thread.
        void setSettled(bool settled) { mSettled = settled; }

        /// Hands the bakers what this walk marked, then moves what is finished into the scene, and
        /// says how many landed. Before anything reads what arrived, because a composite coming
        /// back takes a texture slot and is an arrival like any other.
        std::size_t advance(SceneDesc& scene, Resource::ImageManager& images);

        /// The finished composite in `slot`, or null where nothing here baked one.
        const TerrainComposite* find(Index slot) const;

        /// How many layers or stacks the bakers could not read since the last call, which the
        /// hand-over adds to the frame's unreadable textures.
        std::uint32_t takeUnreadable() { return std::exchange(mUnreadable, 0u); }

        /// Lets go of everything `collect` took. After the upload and not before: what is held
        /// between those two calls is the only copy of the bytes a backend has to read.
        void releaseFinished() { mFinished.clear(); }

    private:
        /// Hands the bakers every chunk the walk wrote that wants flattening and is not already
        /// handed over, off the rows the scene says it wrote and never by scanning the table.
        /// Everything the bake reads is taken here, so no baker reads anything the next walk can
        /// change.
        void gather(const SceneDesc& scene, Resource::ImageManager& images);

        /// Waits until every stack `collect` is due to take has come back.
        void waitFor(std::size_t limit);

        /// How many of the sequences from `mNextTake` are old enough to collect, counted no further
        /// than `limit`. It reads nothing a baker touches, so what a settled frame collects is the
        /// schedule's answer and the wait is only how the frame is made to agree with it.
        std::size_t getDue(std::size_t limit) const;

        /// How many of the sequences from `mNextTake` have come back, counted no further than
        /// `limit`. Under the monitor's lock.
        std::size_t getReady(std::size_t limit) const;

        /// Moves the composites that are due into the scene, in the order they were handed over,
        /// at most `limit` of them, and says how many. Stops at the first sequence that is not due
        /// or has not come back. A composite taken takes a texture slot and goes onto the material
        /// that asked; one whose chunk left the world while it baked is dropped.
        std::size_t collect(SceneDesc& scene, std::size_t limit);

        bool mSettled = false;

        /// Which chunk asked: the material's slot and where its layers sat when it did, so a slot
        /// another chunk took over in the meantime is not handed the first one's ground.
        struct Asked
        {
            Index mMaterial = sNoIndex;
            Run mLayers;

            bool operator==(const Asked& other) const = default;
        };

        /// One chunk's stack as the baker reads it — copied, every part of it, because a bake
        /// outlives the walk that asked for it and the scene may free the chunk's runs meanwhile.
        /// The layers are what `collect` compares against to know the same chunk still stands.
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

            /// Empties the four without giving their room back. The images go: a reference held in
            /// a spare buffer keeps a picture alive for a chunk already collected.
            void reuse()
            {
                reuseKeeping(*this, &Request::mLayers, &Request::mImages, &Request::mMasks, &Request::mMaskRuns);
            }
        };

        /// What came back: the request, and the composite — or none, where every layer was
        /// unreadable and the chunk keeps its stack.
        struct Baked
        {
            Request mRequest;
            std::optional<TerrainComposite> mComposite;

            /// Layers whose image could not be described, and a whole stack that failed to bake:
            /// each is a texture the world stated and the picture lacks, counted where the other
            /// unreadable textures are.
            std::uint32_t mUnreadable = 0;
        };

        /// One thread and everything only it may touch, held apart rather than guarded, because a
        /// bake writes megabytes of working set and a lock around that would put the threads back
        /// in single file.
        struct Baker
        {
            /// The painted light of each texture that was estimated, kept by the file it came from,
            /// because estimating one reads every texel of the largest level and the same handful
            /// of ground textures make every chunk of a region. Not the texture builder's cache,
            /// which drops a description with its slot: a chunk baked from a ground texture no slot
            /// names any more is still a chunk.
            class ShadingCache
            {
            public:
                /// The estimate for `texture`, made once for each `file` and made afresh for every
                /// texture that names none. The reference is good until the next call that names no
                /// file.
                const ShadingMap& estimate(const TextureData& texture, const std::string& file)
                {
                    if (file.empty())
                    {
                        mUnnamed = ShadingMap(texture);
                        return mUnnamed;
                    }

                    return mPainted.try_emplace(file, texture).first->second;
                }

            private:
                std::unordered_map<std::string, ShadingMap> mPainted;

                /// The estimate of a texture with no file to key it by, held only until the next one.
                ShadingMap mUnnamed;
            };

            /// Describes, estimates and flattens one stack, on this baker's thread.
            Baked bake(Request&& request);

            /// This thread's and no other's, because that is where a stack is described. The
            /// estimate is node-based, which is what lets the stack span it.
            ShadingCache mPainted;

            /// What `bake` reads a stack into, and what a bake works in. Held rather than made,
            /// because a fill bakes dozens of chunks.
            std::vector<MipLevel> mLevelScratch;
            std::vector<CompositeLayer> mStackScratch;
            CompositeScratch mScratch;

            /// Last, for the reason `Worker` gives.
            Worker mWorker;
        };

        /// Files `baked` under its sequence, so `mDone` reads in the order the stacks were handed
        /// over however the bakers finished them. Under the monitor's lock.
        void file(Baked&& baked);

        /// Starts every baker, once. Called by the first chunk that asks, so a world that never
        /// reaches distant ground never pays for a thread.
        void startBakers();

        /// A baker's loop: one request at a time, until asked to stop. `Monitor::serve` is the
        /// loop, and this is what it takes and what it does.
        void work(Baker& baker, std::stop_token stop);

        /// The lock between the frame and the bakers, and the two waits across it. It guards
        /// `mPending` and `mDone` and nothing else.
        Monitor mMonitor;

        /// Oldest first, so the bakers take chunks in the order they arrived.
        std::deque<Request> mPending;

        /// By sequence and not by when a bake finished. See `file`.
        std::deque<Baked> mDone;

        /// Which thread everything below belongs to: the frame's, which `advance` asserts.
        OwnedBy mOnFrame;

        /// The next sequence to hand out, and the next to collect. One is written by `gather` and
        /// the other by `collect`, and no baker reads either.
        std::uint64_t mNextGiven = 0;
        std::uint64_t mNextTake = 0;

        /// How many frames have been handed over, which is what a stack's age is measured in.
        std::size_t mFrame = 0;

        /// How many sequences had been handed over by the end of each of the last
        /// `sBakeFrames + 1` frames, by the frame modulo the size. `getDue` reads the entry of
        /// `sBakeFrames` frames ago: every sequence below it is old enough. A count a frame and not
        /// a frame a stack, because the frame a stack was handed over on is monotonic in its
        /// sequence.
        std::array<std::uint64_t, sBakeFrames + 1> mGivenBy{};

        /// Everything handed over and not yet collected, which is what `gather` checks against.
        std::vector<Asked> mAsked;

        /// Collected this frame, by the slot they were given. Emptied by `releaseFinished`.
        std::unordered_map<Index, TerrainComposite> mFinished;

        std::uint32_t mUnreadable = 0;

        /// Refilled per collect rather than built afresh: the frame a composite lands on is not
        /// one to spend an allocation on.
        std::vector<Baked> mTaken;

        /// Requests that have been through the queue and are waiting to carry another chunk, with
        /// the room they grew: a chunk costs four vectors and a crossing gathers dozens. Everything
        /// here has been through `reuse`, so one taken off is empty and holds no image.
        Recycled<Request> mSpare;

        std::string mKey;

        /// Last, for the reason `Worker` gives. Held by pointer so that a baker keeps its
        /// address, which its own thread captured.
        std::vector<std::unique_ptr<Baker>> mBakers;
    };
}
