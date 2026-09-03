#include "compositequeue.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <exception>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>

#include "error.hpp"
#include "frametimes.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
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

    void CompositeQueue::gather(const SceneDesc& scene, Resource::ImageManager& images)
    {
        // **A cleared scene renumbers everything, so nothing here still refers to anything.**
        // `SceneDesc::clear` empties the material table and starts the indices again; a chunk that
        // was waiting is waiting on a material that no longer exists. What is queued is dropped
        // here; what is in flight comes back carrying the reset it was asked under, and `collect`
        // drops it then.
        if (mReset != scene.getResetRevision())
        {
            mReset = scene.getResetRevision();
            mAsked.clear();
            mFinished.clear();

            const std::lock_guard<std::mutex> lock(mMutex);

            // **Taken back rather than dropped**, for the reason `mSpare` gives: a scene replaced
            // outright is exactly when a route is about to gather a region's worth again.
            for (Request& dropped : mPending)
            {
                dropped.reuse();
                mSpare.push_back(std::move(dropped));
            }

            mPending.clear();
            mDone.clear();
        }

        const std::span<const Material> materials = scene.getMaterials();
        for (const Index at : scene.getWrittenMaterials())
        {
            const Material& material = materials[at];
            if (material.mKind != MaterialKind::Terrain || !material.mFlatten || material.mDiffuse != sNoIndex)
                continue;

            const Asked wanted{
                .mMaterial = at,
                .mLayerOffset = material.mLayerOffset,
                .mLayerCount = material.mLayerCount,
            };

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

                const std::lock_guard<std::mutex> lock(mMutex);
                std::erase_if(mPending, [&](Request& one) {
                    if (one.mAsked.mMaterial != at)
                        return false;

                    one.reuse();
                    mSpare.push_back(std::move(one));
                    return true;
                });
            }

            const std::span<const MaterialLayer> layers
                = scene.getLayers().subspan(material.mLayerOffset, material.mLayerCount);

            // **Off the spare list where one has come back.** A request is four vectors and a
            // crossing gathers dozens, so building each here and freeing it in `collect` is a
            // region's worth of allocation twice over. Whatever comes off the list is already
            // empty: `mSpare` says that is what putting one back means.
            Request request;
            if (!mSpare.empty())
            {
                request = std::move(mSpare.back());
                mSpare.pop_back();
            }

            request.mAsked = wanted;
            request.mReset = mReset;
            request.mLayers.assign(layers.begin(), layers.end());
            request.mImages.reserve(layers.size());
            request.mMaskRuns.reserve(layers.size());

            for (const MaterialLayer& layer : layers)
            {
                // Opened here and not on the baker, so the image manager is only ever asked from
                // the thread that owns it; the baker reads what the reference keeps alive.
                request.mImages.push_back(openImage(images, scene.getTextures()[layer.mDiffuse]));

                const std::uint32_t weights = std::uint32_t{ layer.mMaskWidth } * layer.mMaskHeight;
                request.mMaskRuns.push_back(
                    Span{ .mOffset = static_cast<std::uint32_t>(request.mMasks.size()), .mCount = weights });

                const std::span<const float> mask = scene.getMasks().subspan(layer.mMaskOffset, weights);
                request.mMasks.insert(request.mMasks.end(), mask.begin(), mask.end());
            }

            mAsked.push_back(wanted);

            {
                const std::lock_guard<std::mutex> lock(mMutex);
                mPending.push_back(std::move(request));
            }

            if (!mWorker.joinable())
                mWorker = std::jthread([this](std::stop_token stop) { work(stop); });

            mWake.notify_one();
        }
    }

    void CompositeQueue::finish()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mSettled.wait(lock, [&] { return mPending.empty() && mBaking == 0; });
    }

    std::size_t CompositeQueue::collect(SceneDesc& scene, const std::size_t limit)
    {
        mTaken.clear();
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            while (mTaken.size() < limit && !mDone.empty())
            {
                mTaken.push_back(std::move(mDone.front()));
                mDone.pop_front();
            }
        }

        std::size_t finished = 0;
        for (Baked& baked : mTaken)
        {
            const Request& request = baked.mRequest;
            const Asked& asked = request.mAsked;

            // Exactly the entry this was asked as, and not whatever stands under the material now:
            // a slot taken over while this baked has an entry of its own, and that one is waiting
            // on a bake that has not come back yet.
            if (const auto entry = std::find(mAsked.begin(), mAsked.end(), asked); entry != mAsked.end())
                mAsked.erase(entry);

            if (request.mReset != mReset || !baked.mComposite.has_value())
                continue;

            const std::span<const Material> materials = scene.getMaterials();
            assert(asked.mMaterial < materials.size() && "a composite waiting on a material the scene has forgotten");

            const Material& material = materials[asked.mMaterial];

            // **What it baked has to still be what stands there.** A chunk can leave the world in
            // the frames a composite takes, and the slot it stood in can be taken over by another;
            // handing this to whatever holds the slot now would put one hillside's ground on
            // another's. The layers themselves are compared and not only where they sit, because a
            // run given back is handed out again to the next chunk that fits it.
            const bool wanted = material.mKind == MaterialKind::Terrain && material.mFlatten
                && material.mDiffuse == sNoIndex && material.mLayerOffset == asked.mLayerOffset
                && material.mLayerCount == asked.mLayerCount
                && std::ranges::equal(
                    request.mLayers, scene.getLayers().subspan(material.mLayerOffset, material.mLayerCount));

            if (!wanted)
                continue;

            nameComposite(mKey, asked.mMaterial);

            Material given = material;
            given.mDiffuse = scene.addBakedTexture(mKey);
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
            mSpare.push_back(std::move(baked.mRequest));
        }

        mTaken.clear();
        return finished;
    }

    const TerrainComposite* CompositeQueue::find(const Index slot) const
    {
        const auto found = mFinished.find(slot);
        return found == mFinished.end() ? nullptr : &found->second;
    }

    void CompositeQueue::work(std::stop_token stop)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        while (mWake.wait(lock, stop, [&] { return !mPending.empty(); }))
        {
            if (stop.stop_requested())
                return;

            Request request = std::move(mPending.front());
            mPending.pop_front();
            ++mBaking;
            lock.unlock();

            Baked baked = bake(std::move(request));

            lock.lock();
            --mBaking;
            mDone.push_back(std::move(baked));
            mSettled.notify_all();
        }
    }

    CompositeQueue::Baked CompositeQueue::bake(Request&& request)
    {
        Baked baked{ .mRequest = std::move(request) };
        const Request& asked = baked.mRequest;

        // **Reserved before anything points into it.** Every description below spans `levels`, so
        // a reallocation part way through would leave the bake reading where the earlier layers
        // used to be.
        std::size_t count = 0;
        for (const osg::ref_ptr<const osg::Image>& image : asked.mImages)
            count += image != nullptr ? image->getNumMipmapLevels() : 0;

        std::vector<MipLevel> levels;
        levels.reserve(count);

        std::vector<CompositeLayer> stack;
        stack.reserve(asked.mLayers.size());

        for (std::size_t index = 0; index < asked.mLayers.size(); ++index)
        {
            const osg::Image* image = asked.mImages[index].get();
            if (image == nullptr)
                continue;

            std::optional<TextureData> described;
            try
            {
                described = describeImage(*image, levels);
            }
            catch (const Error&)
            {
                // A file in a format this renderer does not upload is a layer with nothing to
                // flatten, and the shader shades the chunk without it either way.
                continue;
            }

            const MaterialLayer& layer = asked.mLayers[index];
            const Span mask = asked.mMaskRuns[index];

            stack.push_back(CompositeLayer{
                .mDiffuse = *described,
                .mShading = mPainted.estimate(*described, image->getFileName()).getValues(),
                .mDiffuseTransform = layer.mDiffuseTransform,
                .mMask = std::span<const float>(asked.mMasks).subspan(mask.mOffset, mask.mCount),
                .mMaskWidth = layer.mMaskWidth,
                .mMaskHeight = layer.mMaskHeight,
                .mMaskTransform = layer.mMaskTransform,
            });
        }

        // Every layer unreadable is a chunk with nothing to flatten. It keeps its stack, which is
        // what it was already shading from, and asks again no more than the walk does.
        if (stack.empty())
            return baked;

        // **The only place a bake's cost is visible.** It happens on this thread and off every
        // frame, so no frame timer reaches it and a profile can say what share of a run it was but
        // never what one chunk cost. Verbose, because a crossing queues dozens.
        const auto started = std::chrono::steady_clock::now();

        try
        {
            baked.mComposite.emplace(stack, sCompositeExtent, sCompositeDelight);

            // The first layer's mask, which for a terrain chunk is every layer's: the blend maps of
            // one chunk are one grid, and what the number says is how finely the stack is cut.
            Log(Debug::Verbose)
                << "composite bake: " << stack.size() << " layers, mask " << stack.front().mMaskWidth << "x"
                << stack.front().mMaskHeight << ", "
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
                << " ms";
        }
        catch (const std::exception& error)
        {
            // Nothing may leave a thread, and a chunk that could not be flattened is a chunk that
            // shades from its stack — a cost per hit, not a picture lost.
            Log(Debug::Error) << "a terrain composite could not be baked: " << error.what();
        }

        return baked;
    }
}
