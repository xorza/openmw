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

#include "scenedesc.hpp"
#include "shadingmap.hpp"
#include "spanallocator.hpp"
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

        /// Hands the baker every chunk the walk wrote that wants flattening and is not already
        /// handed over.
        ///
        /// **Off the rows the scene says it wrote**, which is the only place a chunk wanting a
        /// composite can appear; the table itself is never scanned. Everything the bake reads — the
        /// images, the weights, the transforms — is taken here, so the thread reads nothing the next
        /// walk can change.
        void gather(const SceneDesc& scene, Resource::ImageManager& images);

        /// Waits until nothing handed over is still baking.
        ///
        /// **For a caller with no next frame.** A harness that stages a region, renders one frame
        /// and stops has nowhere to put a bake that finishes later, and would photograph ground no
        /// player sees.
        void finish();

        /// Moves at most `limit` finished composites into the scene, oldest first.
        ///
        /// A composite taken takes a texture slot — which puts it among the scene's arrivals, so the
        /// upload that follows carries it — and goes onto the material that asked. One whose chunk
        /// left the world while it baked, or whose scene was replaced outright, is dropped instead.
        ///
        /// @return how many were taken.
        std::size_t collect(SceneDesc& scene, std::size_t limit);

        /// The finished composite in `slot`, or null where nothing here baked one.
        const TerrainComposite* find(Index slot) const;

        /// Lets go of everything `collect` took. **After the upload and not before**: what is held
        /// between those two calls is the only copy of the bytes a backend has to read.
        void releaseFinished() { mFinished.clear(); }

        /// How many chunks are handed over and not yet collected, which is what says whether a
        /// collect has anything to wait for.
        std::size_t getWaitingCount() const { return mAsked.size(); }

    private:
        /// Which chunk asked: the material's slot and where its layers sat when it did.
        ///
        /// What the frame side remembers of everything handed over and not yet collected, without
        /// taking the lock — and what a bake is matched back to, so a slot another chunk took over
        /// in the meantime is not handed the first one's ground.
        struct Asked
        {
            Index mMaterial = sNoIndex;
            Index mLayerOffset = 0;
            Index mLayerCount = 0;

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

            /// The reset this was gathered under, so a scene replaced outright takes it with it.
            std::uint64_t mReset = 0;

            std::vector<MaterialLayer> mLayers;

            /// Parallel to `mLayers`, null where the layer's file could not be opened.
            std::vector<osg::ref_ptr<const osg::Image>> mImages;

            /// Every layer's weights end to end, and where each layer's run sits in them — a count
            /// of nought for a layer that covers the chunk.
            std::vector<float> mMasks;
            std::vector<Span> mMaskRuns;
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

        /// The painted light of a ground texture, estimated once per file for the life of the queue.
        ///
        /// **Node-based and keyed by the file, because the stack spans these and the same handful of
        /// ground textures make every chunk of a region.** Estimating one reads every texel of a
        /// texture's largest level, which is the 5% of a crossing's CPU `texturebuilder.hpp` names.
        /// On the baker's thread only.
        const ShadingMap& estimate(const TextureData& texture, const std::string& file);

        /// Guards `mPending`, `mDone` and `mBaking` — everything the two threads share.
        std::mutex mMutex;

        /// Woken by a request arriving, or by the stop.
        std::condition_variable_any mWake;

        /// Woken by a bake finishing, which is what `finish` waits for.
        std::condition_variable mSettled;

        /// Oldest first, so the baker finishes chunks in the order they arrived.
        std::deque<Request> mPending;
        std::deque<Baked> mDone;
        std::size_t mBaking = 0;

        /// Everything handed over and not yet collected, which is what `gather` checks against.
        std::vector<Asked> mAsked;

        /// Collected this frame, by the slot they were given. Emptied by `releaseFinished`.
        std::unordered_map<Index, TerrainComposite> mFinished;

        /// The reset `gather` last saw, so a cleared scene takes what was waiting with it.
        std::uint64_t mReset = 0;

        /// Refilled per collect rather than built afresh: the frame a composite lands on is not
        /// one to spend an allocation on.
        std::vector<Baked> mTaken;
        std::string mKey;

        std::unordered_map<std::string, ShadingMap> mPainted;

        /// The estimate of a texture with no file to key it by, held only until the next one.
        ShadingMap mUnnamed;

        /// **Last, so it is joined first.** A member declared above it would be destroyed while
        /// the baker was still reading it; the stop the join begins with is what wakes the wait.
        /// Started by the first chunk that asks rather than with the queue: every picture inside
        /// the interface has an uploader and a queue of its own, and a doll never asks.
        std::jthread mWorker;
    };
}
