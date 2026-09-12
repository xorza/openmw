#include "sceneuploader.hpp"

#include <chrono>
#include <span>

#include "compositequeue.hpp"
#include "frameclock.hpp"
#include "renderer.hpp"
#include "scenedesc.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// Hands over the texture slots the scene has given up, and says how many there were.
        std::size_t dropFreed(Renderer& renderer, SceneSlot slot, const SceneDesc& scene)
        {
            const std::span<const Index> freed = scene.textures().getFreed();
            if (!freed.empty())
                renderer.dropTextures(slot, freed);

            return freed.size();
        }
    }

    SceneUpload SceneUploader::hand(Renderer& renderer, const Handing& handing)
    {
        const SceneSlot slot = handing.mSlot;
        SceneDesc& scene = handing.mScene;
        Resource::ImageManager& images = handing.mImages;
        CompositeQueue* const composites = handing.mComposites;
        const CellHolds* const readings = handing.mReadings;
        FrameSpend* const spend = handing.mSpend;

        // Timed into a row nobody reads where the caller handed none, so the three stretches below
        // are written once each rather than guarded three times.
        FrameSpend unread;
        FrameSpend& timed = spend != nullptr ? *spend : unread;

        // Whether the backend holds this scene in this slot: appending onto a slot something else
        // filled would begin the descriptions past the end of this scene's own table.
        const SceneHeld held = renderer.describeHeld(slot);
        const bool mine = mBuilt && held.mBuilt;

        // Here rather than where a walk ends, because a scene can be walked more than once. The
        // game walks its precipitation beside its world, and a light met by the second walk would be
        // outside an order the first had settled. This is the one point every path passes and the
        // last before anything reads them. `SceneDesc::orderLights` says what depends on it.
        scene.orderLights();

        SceneUpload done;

        // The three halves of the hand-over are timed apart, because a frame that stalls stalls
        // in one of them and the profile cannot say which: an arrival frame's time is in the driver,
        // which carries no frame pointer. `Rtx::Timing::Bake` says the rest.
        const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();

        const std::size_t baked = composites != nullptr ? composites->advance(scene, images) : 0;
        done.mUnreadable = composites != nullptr ? composites->takeUnreadable() : 0;

        const std::chrono::steady_clock::time_point gathered = std::chrono::steady_clock::now();
        timed.at(Timing::Bake) = since(began, gathered);

        // After the two calls above, because both rewrite what the spans reach.
        const SceneDesc& tables = scene;

        // Geometry the walk has not met before has no bottom-level structure and no uploaded
        // texture, which is a cell change and not a frame. A revision and not a set of table sizes,
        // because a crossing loses one cell as it gains another. A frame that only finished a bake
        // is an arrival for everything below even though nothing was walked.
        const bool arrived = !mine || tables.getStructureRevision() != held.mStructureRevision || baked > 0;

        if (!arrived)
        {
            const std::chrono::steady_clock::time_point told = std::chrono::steady_clock::now();

            // A departure with nothing arriving is the ordinary way to leave a region, and it is
            // the frame that must not wait for an arrival to give the memory back: walking away from
            // a ring frees its slots and nothing takes them over until the walk reaches the far side
            // of the next one.
            done.mDropped = dropFreed(renderer, slot, tables);

            // Placed before the lists are forgotten, because placing is what consumes the meshes
            // that went: their structures are destroyed and their storage given back there. Clearing
            // first would hand the renderer an empty list and hold a departed ring's structures
            // until something arrived to take the slots over.
            renderer.placeScene(slot, tables);

            timed.at(Timing::Upload) = since(told, std::chrono::steady_clock::now());
            done.mKind = SceneUpload::Kind::Placed;
        }
        else
        {
            // Read only across the call below: `TextureData` carries spans into `mTextures`. The
            // whole table where there is nothing to append to, and the arrivals otherwise.
            if (!mine)
                mTextures.describeAll(tables, images, composites, readings);
            else
                mTextures.describe(tables, images, tables.textures().getArrived(), composites, readings);

            const std::chrono::steady_clock::time_point described = std::chrono::steady_clock::now();
            timed.at(Timing::Textures) = since(gathered, described);

            done.mDescribed = mTextures.getDescriptions().size();
            done.mUnreadable += mTextures.getUnreadable();

            if (!mine)
            {
                renderer.setScene(slot, tables, mTextures.getDescriptions());
                mBuilt = true;
                done.mKind = SceneUpload::Kind::Rebuilt;
            }
            else
            {
                // Order against the arrivals is free — `SceneDesc` keeps the two lists disjoint —
                // and first is where the memory is given back soonest. A build from nothing needs
                // none of this: the array holds no image of what went.
                done.mDropped = dropFreed(renderer, slot, tables);
                renderer.extendScene(slot, tables, mTextures.getDescriptions());
                done.mKind = SceneUpload::Kind::Extended;
            }

            timed.at(Timing::Upload) = since(described, std::chrono::steady_clock::now());
        }

        // One tail, because all three hand-overs end the same way: each has uploaded, so each is
        // done with the scene's arrivals and with the queue's bytes. Said per branch instead, none of
        // it is owed by any one branch in particular, so a branch written without a line of it looks
        // finished — and the release is the line a frame that baked a composite never reaches.
        scene.clearArrivals();

        // After the upload and not before. Between the collect and here, what the queue holds is
        // the only copy of a composite's bytes; a region's worth is fifty megabytes, and keeping
        // them past the frame that read them would be paying for one picture twice.
        if (composites != nullptr)
            composites->releaseFinished();

        return done;
    }
}
