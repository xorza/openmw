#include "sceneuploader.hpp"

#include <span>

#include "compositequeue.hpp"
#include "renderer.hpp"
#include "scenedesc.hpp"
#include "texturebuilder.hpp"
#include "wavespectrum.hpp"

namespace Rtx
{
    namespace
    {
        /// Hands over the texture slots the scene has given up, and says how many there were.
        std::size_t dropFreed(SceneSink& renderer, SceneSlot slot, const SceneTables& scene)
        {
            const std::span<const Index> freed = scene.mTextures.getFreed();
            if (!freed.empty())
                renderer.dropTextures(slot, freed);

            return freed.size();
        }
    }

    bool SceneUploader::recognises(
        const SceneSink& renderer, const SceneSlot slot, const SceneDesc& scene, std::uint32_t textures) const
    {
        return mRenderer == &renderer && mSlot == slot && mScene == &scene && mUploaded == textures;
    }

    SceneUpload SceneUploader::hand(SceneSink& renderer, const SceneSlot slot, SceneDesc& scene,
        Resource::ImageManager& images, CompositeQueue* const composites, const SeaState& sea)
    {
        const bool mine = recognises(renderer, slot, scene, renderer.getTextureCount(slot));

        // **Here rather than where a walk ends, because a scene can be walked more than once.** The
        // game walks its precipitation beside its world, and a light met by the second walk would be
        // outside an order the first had settled. This is the one point every path passes and the
        // last before anything reads them. `SceneDesc::orderLights` says what depends on it.
        scene.orderLights();

        const std::size_t baked = composites != nullptr ? composites->advance(scene, images) : 0;

        // **After the two calls above, because both rewrite what the spans reach.**
        const SceneTables tables = scene.getTables();

        // Geometry the walk has not met before has no bottom-level structure and no uploaded
        // texture. **Which is a cell change and a load, not a frame** — a door opening moves
        // instances the walk already knows.
        //
        // A frame that only finished a bake has no new geometry and a new texture, which is an
        // arrival for everything below even though nothing was walked.
        const bool arrived = !mine || tables.getStructureRevision() != mBuilt || baked > 0;

        SceneUpload done;

        if (!arrived)
        {
            // **A departure with nothing arriving is the ordinary way to leave a region**, and it is
            // the frame that must not wait for an arrival to give the memory back: walking away from
            // a ring frees its slots and nothing takes them over until the walk reaches the far side
            // of the next one.
            done.mDropped = dropFreed(renderer, slot, tables);

            // **Placed before the lists are forgotten**, because placing is what consumes the meshes
            // that went: their structures are destroyed and their storage given back there. Clearing
            // first would hand the renderer an empty list and hold a departed ring's structures
            // until something arrived to take the slots over.
            renderer.placeScene(slot, tables, sea);
            done.mKind = SceneUpload::Kind::Placed;
        }
        else
        {
            // Read only across the call below: `TextureData` carries spans into `mTextures`, and
            // both `extendScene` and `setScene` have finished reading them when they return. What
            // the loader holds after that is capacity for the next arrival.
            //
            // **The whole table where there is nothing to append to, and the arrivals
            // otherwise.** An uploader that has not built this pair makes the array from nothing, so
            // what it wants is the table in its own order; a frame that grew wants the slots that
            // were written and no others, wherever in the table they sit.
            if (!mine)
                mTextures.describeAll(tables, images, composites);
            else
                mTextures.describe(tables, images, tables.mTextures.getArrived(), composites);

            done.mDescribed = mTextures.getDescriptions().size();
            done.mUnreadable = mTextures.getUnreadable();

            if (!mine)
            {
                renderer.setScene(slot, tables, mTextures.getDescriptions(), sea);
                done.mKind = SceneUpload::Kind::Rebuilt;
            }
            else
            {
                // Order against the arrivals is free — `SceneDesc` keeps the two lists disjoint —
                // and first is where the memory is given back soonest. A build from nothing needs
                // none of this: the array holds no image of what went.
                done.mDropped = dropFreed(renderer, slot, tables);
                renderer.extendScene(slot, tables, mTextures.getDescriptions(), sea);
                done.mKind = SceneUpload::Kind::Extended;
            }
        }

        // **One tail, because all three hand-overs end the same way**: each has uploaded, so each is
        // done with the scene's arrivals and with the queue's bytes, and each leaves the uploader
        // describing what it just handed over. Said per branch instead, none of it is owed by any
        // one branch in particular, so a branch written without a line of it looks finished — and
        // the release is the line a frame that baked a composite never reaches.
        //
        // Every field below is already what it is being set to wherever the placing branch ran, since
        // that branch is reached only where `mine` held and nothing on it moves a revision or grows
        // the renderer's table.
        scene.clearArrivals();

        // **After the upload and not before.** Between the collect and here, what the queue holds is
        // the only copy of a composite's bytes; a region's worth is fifty megabytes, and keeping
        // them past the frame that read them would be paying for one picture twice.
        if (composites != nullptr)
            composites->releaseFinished();

        mRenderer = &renderer;
        mSlot = slot;
        mScene = &scene;
        mUploaded = renderer.getTextureCount(slot);
        mBuilt = tables.getStructureRevision();
        return done;
    }
}
