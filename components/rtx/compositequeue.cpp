#include "compositequeue.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <exception>
#include <memory>
#include <span>
#include <thread>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>

#include "error.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// How many threads flatten stacks.
        ///
        /// **Four, because the sum reads far more than it computes.** A bake is nearly all summing
        /// the layer stack into 512² texels, which divides across threads, and the memory contention
        /// between them is what stops the division paying past four.
        ///
        /// **A quarter of the machine and never more than four**, so a smaller one keeps the cores
        /// the frame, the cell reader and the driver are on.
        std::size_t bakerCount()
        {
            const unsigned int cores = std::thread::hardware_concurrency();
            return std::clamp<std::size_t>(cores / 4, 1, 4);
        }

        /// The key a chunk's composite is found under.
        ///
        /// **The material's own slot, because one material is one chunk.** The extractor keys a
        /// terrain material on the state set it came from, so the two are already one to one; a
        /// material that is retired takes its composite's slot with it, and one that takes the slot
        /// over is a different chunk asking for a different bake under the same name — which is
        /// exactly right, because it wants the slot overwritten.
        void nameComposite(std::string& key, Index material)
        {
            std::array<char, 16> digits{};
            const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), material, 16);

            key.assign("chunk/");
            key.append(digits.data(), written.ptr);
        }
    }

    std::size_t CompositeQueue::advance(SceneDesc& scene, Resource::ImageManager& images)
    {
        mOnFrame.check();

        ++mFrame;

        gather(scene, images);
        mGivenBy[mFrame % mGivenBy.size()] = mNextGiven;

        if (mSettled)
            waitFor(sCompositesPerFrame);

        return collect(scene, sCompositesPerFrame);
    }

    void CompositeQueue::gather(const SceneDesc& scene, Resource::ImageManager& images)
    {
        const std::span<const Material> materials = scene.materials().getRows();
        for (const Index at : scene.materials().getWritten())
        {
            const Material& material = materials[at];
            if (material.mKind != MaterialKind::Terrain || !material.mFlatten || material.mDiffuse != sNoIndex)
                continue;

            const Asked wanted{ .mMaterial = at, .mLayers = material.mLayers };

            const auto asked
                = std::find_if(mAsked.begin(), mAsked.end(), [&](const Asked& one) { return one.mMaterial == at; });
            if (asked != mAsked.end() && *asked == wanted)
                continue;

            // A slot taken over by another chunk while its predecessor was baking: what is half done
            // is a picture of ground that has gone. What is still queued is dropped here; what is
            // in flight or finished is dropped by `collect`, which checks the layers it baked against
            // the layers that stand.
            if (asked != mAsked.end())
            {
                mAsked.erase(asked);

                mMonitor.under([&] {
                    std::erase_if(mPending, [&](Request& one) {
                        if (one.mAsked.mMaterial != at)
                            return false;

                        // **Filed rather than dropped**, because a sequence that never arrived would
                        // stop `collect` at it for the rest of the run. It comes back holding nothing,
                        // which is what `collect` already does with a bake whose chunk has gone.
                        one.reuse();
                        file(Baked{ .mRequest = std::move(one) });
                        return true;
                    });
                });
            }

            const std::span<const MaterialLayer> layers = material.mLayers.in(scene.materials().getLayers());

            // **Off the spare list where one has come back.** A request is four vectors and a
            // crossing gathers dozens, so building each here and freeing it in `collect` is a
            // region's worth of allocation twice over. Whatever comes off the list is already
            // empty: `mSpare` says that is what putting one back means.
            Request request = mSpare.take();
            request.mAsked = wanted;
            request.mSequence = mNextGiven++;
            request.mLayers.assign(layers.begin(), layers.end());
            request.mImages.reserve(layers.size());
            request.mMaskRuns.reserve(layers.size());

            for (const MaterialLayer& layer : layers)
            {
                // Opened here and not on a baker, so the image manager is only ever asked from the
                // thread that owns it; a baker reads what the reference keeps alive.
                request.mImages.push_back(openImage(images, scene.textures().getPaths()[layer.mDiffuse]));

                request.mMaskRuns.push_back(
                    Run{ .mOffset = static_cast<std::uint32_t>(request.mMasks.size()), .mCount = layer.mMask.mCount });

                const std::span<const float> mask = layer.mMask.in(scene.materials().getMasks());
                request.mMasks.insert(request.mMasks.end(), mask.begin(), mask.end());
            }

            mAsked.push_back(wanted);

            startBakers();

            mMonitor.give([&] { mPending.push_back(std::move(request)); });
        }
    }

    void CompositeQueue::waitFor(const std::size_t limit)
    {
        // **A monitor that answered false is one every baker has left**, and `collect` asks
        // `rethrowFailure` for the reason on the line after this.
        mMonitor.await([&] {
            const std::size_t due = getDue(limit);
            return getReady(due) >= due;
        });
    }

    std::size_t CompositeQueue::getDue(const std::size_t limit) const
    {
        if (mFrame < sBakeFrames)
            return 0;

        // Every sequence handed over by the end of that frame, less those taken since — which
        // never overtakes it, because a frame takes no more than its own count of due.
        const std::uint64_t given = mGivenBy[(mFrame - sBakeFrames) % mGivenBy.size()];
        assert(given >= mNextTake && "more taken than were due");

        return static_cast<std::size_t>(std::min<std::uint64_t>(limit, given - mNextTake));
    }

    std::size_t CompositeQueue::getReady(const std::size_t limit) const
    {
        std::size_t run = 0;
        while (run < limit && run < mDone.size() && mDone[run].mRequest.mSequence == mNextTake + run)
            ++run;

        return run;
    }

    void CompositeQueue::file(Baked&& baked)
    {
        const std::uint64_t sequence = baked.mRequest.mSequence;
        const auto after = std::upper_bound(mDone.begin(), mDone.end(), sequence,
            [](const std::uint64_t one, const Baked& other) { return one < other.mRequest.mSequence; });

        mDone.insert(after, std::move(baked));
    }

    void CompositeQueue::startBakers()
    {
        if (!mBakers.empty())
            return;

        const std::size_t count = bakerCount();
        mBakers.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            Baker& baker = *mBakers.emplace_back(std::make_unique<Baker>());
            baker.mWorker.start([this, &baker](std::stop_token stop) { work(baker, stop); });
        }
    }

    std::size_t CompositeQueue::collect(SceneDesc& scene, const std::size_t limit)
    {
        // **Asked before anything is taken.** A baker that threw left the queue closed, and a
        // frame that read what came back before it before it asked would report a short collect
        // rather than the failure under it.
        mMonitor.rethrowFailure();

        mTaken.clear();
        const std::size_t due = getDue(limit);
        mMonitor.under([&] {
            // **In sequence and never in whatever order the bakers finished**, which is what makes
            // the frame a composite lands on the schedule's answer. `setSettled` says why.
            while (mTaken.size() < due && !mDone.empty() && mDone.front().mRequest.mSequence == mNextTake)
            {
                mTaken.push_back(std::move(mDone.front()));
                mDone.pop_front();
                ++mNextTake;
            }
        });

        std::size_t finished = 0;
        for (Baked& baked : mTaken)
        {
            const Request& request = baked.mRequest;
            const Asked& asked = request.mAsked;
            mUnreadable += baked.mUnreadable;

            // Exactly the entry this was asked as, and not whatever stands under the material now:
            // a slot taken over while this baked has an entry of its own, and that one is waiting
            // on a bake that has not come back yet.
            if (const auto entry = std::find(mAsked.begin(), mAsked.end(), asked); entry != mAsked.end())
                mAsked.erase(entry);

            if (!baked.mComposite.has_value())
                continue;

            const std::span<const Material> materials = scene.materials().getRows();
            assert(asked.mMaterial < materials.size() && "a composite waiting on a material the scene has forgotten");

            const Material& material = materials[asked.mMaterial];

            // **What it baked has to still be what stands there.** A chunk can leave the world in
            // the frames a composite takes, and the slot it stood in can be taken over by another;
            // handing this to whatever holds the slot now would put one hillside's ground on
            // another's. The layers themselves are compared and not only where they sit, because a
            // run given back is handed out again to the next chunk that fits it.
            const bool wanted = material.mKind == MaterialKind::Terrain && material.mFlatten
                && material.mDiffuse == sNoIndex && material.mLayers == asked.mLayers
                && std::ranges::equal(request.mLayers, material.mLayers.in(scene.materials().getLayers()));

            if (!wanted)
                continue;

            nameComposite(mKey, asked.mMaterial);

            Material given = material;
            given.mDiffuse = scene.textures().addBaked(mKey);
            scene.setMaterial(asked.mMaterial, given);

            mFinished.insert_or_assign(given.mDiffuse, std::move(*baked.mComposite));
            ++finished;
        }

        // **The images go and the buffers stay.** What a request held is a picture of ground already
        // baked, and holding it past here would be a second copy of every layer a region uses; the
        // vectors themselves are room the next chunk would otherwise ask the allocator for.
        for (Baked& baked : mTaken)
        {
            baked.mRequest.reuse();
            mSpare.give(std::move(baked.mRequest));
        }

        mTaken.clear();
        return finished;
    }

    const TerrainComposite* CompositeQueue::find(const Index slot) const
    {
        const auto found = mFinished.find(slot);
        return found == mFinished.end() ? nullptr : &found->second;
    }

    void CompositeQueue::work(Baker& baker, std::stop_token stop)
    {
        Request request;

        mMonitor.serve(
            stop, [&] { return !mPending.empty(); },
            [&] {
                request = std::move(mPending.front());
                mPending.pop_front();
            },
            [&](std::stop_token) {
                Baked baked = baker.bake(std::move(request));
                mMonitor.hand([&] { file(std::move(baked)); });
            });
    }

    CompositeQueue::Baked CompositeQueue::Baker::bake(Request&& request)
    {
        Baked baked{ .mRequest = std::move(request) };
        const Request& asked = baked.mRequest;

        // Reserved before anything points into it: every description below spans `mLevelScratch`,
        // so a reallocation part way through would leave the earlier layers' spans dangling.
        std::size_t count = 0;
        for (const osg::ref_ptr<const osg::Image>& image : asked.mImages)
            count += image != nullptr ? image->getNumMipmapLevels() : 0;

        mLevelScratch.clear();
        mLevelScratch.reserve(count);

        mStackScratch.clear();
        mStackScratch.reserve(asked.mLayers.size());

        for (std::size_t index = 0; index < asked.mLayers.size(); ++index)
        {
            const osg::Image* image = asked.mImages[index].get();
            if (image == nullptr)
                continue;

            std::optional<TextureData> described;
            try
            {
                described = describeImage(*image, mLevelScratch);
            }
            catch (const Error&)
            {
                // A file in a format this renderer does not upload is a layer with nothing to
                // flatten; the shader shades the chunk without it, and the frame counts it.
                ++baked.mUnreadable;
                continue;
            }

            const MaterialLayer& layer = asked.mLayers[index];
            const Run mask = asked.mMaskRuns[index];

            mStackScratch.push_back(CompositeLayer{
                .mDiffuse = *described,
                .mShading = mPainted.estimate(*described, image->getFileName()).getValues(),
                .mDiffuseTransform = layer.mDiffuseTransform,
                .mMask = mask.in(std::span<const float>(asked.mMasks)),
                .mMaskWidth = layer.mMaskWidth,
                .mMaskHeight = layer.mMaskHeight,
                .mMaskTransform = layer.mMaskTransform,
            });
        }

        // Every layer unreadable is a chunk with nothing to flatten. It keeps its stack, which is
        // what it was already shading from, and asks again no more than the walk does.
        if (mStackScratch.empty())
            return baked;

        try
        {
            baked.mComposite.emplace(mStackScratch, sCompositeExtent, sCompositeDelight, mScratch);
        }
        catch (const std::exception& error)
        {
            // Nothing may leave a thread, and a chunk that could not be flattened is a chunk that
            // shades from its stack — a cost per hit, not a picture lost.
            Log(Debug::Error) << "a terrain composite could not be baked: " << error.what();
            ++baked.mUnreadable;
        }

        return baked;
    }
}
