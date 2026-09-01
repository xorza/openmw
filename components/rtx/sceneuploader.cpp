#include "sceneuploader.hpp"

#include <chrono>
#include <limits>
#include <span>

#include <components/debug/debuglog.hpp>

#include "frametimes.hpp"
#include "renderer.hpp"
#include "scenedesc.hpp"
#include "scenemasks.hpp"
#include "texturebuilder.hpp"
#include "wavespectrum.hpp"

namespace Rtx
{
    namespace
    {
        /// Hands over the texture slots the scene has given up, and says how many there were.
        std::size_t dropFreed(Renderer& renderer, std::uint32_t slot, const SceneDesc& scene)
        {
            const std::span<const Index> freed = scene.getFreedTextures();
            if (!freed.empty())
                renderer.dropTextures(slot, freed);

            return freed.size();
        }
    }

    bool SceneUploader::recognises(
        const Renderer& renderer, std::uint32_t slot, const SceneDesc& scene, std::uint32_t textures) const
    {
        return mRenderer == &renderer && mSlot == slot && mScene == &scene && mUploaded == textures;
    }

    SceneUpload SceneUploader::hand(
        Renderer& renderer, std::uint32_t slot, SceneDesc& scene, Resource::ImageManager& images, const SeaState& sea)
    {
        const bool mine = recognises(renderer, slot, scene, renderer.getTextureCount(slot));

        // **Where a crossing frame goes.** The game reports this whole call as `place ms` and it is
        // the largest row of a crossing; without the three figures below, which of its parts cost
        // the frame is guesswork. Two clock reads on a frame that places nothing.
        const auto entered = std::chrono::steady_clock::now();

        // **Here rather than where a walk ends, because a scene can be walked more than once.** The
        // game walks its precipitation beside its world, and a light met by the second walk would be
        // outside an order the first had settled. This is the one point every path passes and the
        // last before anything reads them. `SceneDesc::orderLights` says what depends on it.
        scene.orderLights();

        // **Before anything reads what arrived, because a composite coming back is an arrival.**
        // The queue hands its baker whatever the walk marked for flattening and takes back a
        // bounded number of what the baker finished; a composite taken here took a texture slot on
        // the way, so the upload below carries it without knowing it was ever waiting. A caller with
        // no next frame waits for the baker first and takes everything.
        mComposites.gather(scene, images);

        if (mStaged || mSettled)
            mComposites.finish();

        const std::size_t baked
            = mComposites.collect(scene, mStaged ? std::numeric_limits<std::size_t>::max() : sCompositesPerFrame);

        const auto collected = std::chrono::steady_clock::now();

        // Geometry the walk has not met before has no bottom-level structure and no uploaded
        // texture. **Which is a cell change and a load, not a frame** — a door opening moves
        // instances the walk already knows.
        // A frame that only finished a bake has no new geometry and a new texture, which is an
        // arrival for everything below even though nothing was walked.
        const bool arrived = !mine || scene.getStructureRevision() != mBuilt || baked > 0;

        // **Grown, or replaced?** `clear` empties every table and starts the indices again, so
        // everything built from them has to be built again; anything short of that is an append, and
        // appending is what keeps a cell boundary from costing a fifth of a second. A renderer this
        // has not built has nothing to append to, whatever it happens to be holding.
        const bool reset = !mine || scene.getResetRevision() != mReset;

        if (!arrived)
        {
            // **A departure with nothing arriving is the ordinary way to leave a region**, and it is
            // the frame that must not wait for an arrival to give the memory back: walking away from
            // a ring frees its slots and nothing takes them over until the walk reaches the far side
            // of the next one.
            SceneUpload left;
            left.mDropped = dropFreed(renderer, slot, scene);

            // **Placed before the lists are forgotten**, because placing is what consumes the meshes
            // that went: their structures are destroyed and their storage given back there. Clearing
            // first would hand the renderer an empty list and hold a departed ring's structures
            // until something arrived to take the slots over.
            renderer.placeScene(slot, scene, sea);
            scene.clearArrivals();

            // **After the upload and not before.** Between the collect and here, what the queue holds
            // is the only copy of a composite's bytes; a region's worth is fifty megabytes, and
            // keeping them past the frame that read them would be paying for one picture twice.
            mComposites.releaseFinished();
            return left;
        }

        // Held only across the call: `TextureData` carries spans into this, and both `extendScene`
        // and `setScene` have finished reading them when they return.
        //
        // **Everything on a reset and the arrivals otherwise.** A reset builds the array from
        // nothing, so what it wants is the table in its own order; a frame that grew wants the slots
        // that were written and no others, wherever in the table they sit.
        const SceneTextures textures = reset ? SceneTextures(scene, images, &mComposites)
                                             : SceneTextures(scene, images, scene.getArrivedTextures(), &mComposites);

        const auto described = std::chrono::steady_clock::now();

        SceneUpload done;
        done.mDescribed = textures.getDescriptions().size();
        done.mUnreadable = textures.getUnreadable();

        if (reset)
        {
            renderer.setScene(slot, scene, textures.getDescriptions(), sea);
            mReset = scene.getResetRevision();
            done.mKind = SceneUpload::Kind::Rebuilt;
        }
        else
        {
            // Order against the arrivals is free — `SceneDesc` keeps the two lists disjoint — and
            // first is where the memory is given back soonest. A reset needs none of this: the array
            // is made again from nothing and holds no image of what went.
            done.mDropped = dropFreed(renderer, slot, scene);

            // **What the arriving meshes wear, which is not what arrived with them.** `SceneMasks`
            // says why the two differ and what the difference cost.
            const SceneMasks masks(scene, images, scene.getArrivedMeshes(), textures.getDescriptions());
            done.mMasksOpened = masks.getOpened();

            renderer.extendScene(slot, scene, textures.getDescriptions(), masks.getDescriptions(), sea);
            done.mKind = SceneUpload::Kind::Extended;
        }

        scene.clearArrivals();

        const auto handed = std::chrono::steady_clock::now();

        Log(Debug::Verbose) << "scene hand: " << since(entered, collected) << " ms on composites, "
                            << since(collected, described) << " describing " << done.mDescribed << " textures, "
                            << since(described, handed) << " in the renderer";

        mRenderer = &renderer;
        mSlot = slot;
        mScene = &scene;
        mUploaded = renderer.getTextureCount(slot);
        mBuilt = scene.getStructureRevision();
        return done;
    }
}
