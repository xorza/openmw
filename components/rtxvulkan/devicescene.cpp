#include "devicescene.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include <components/rtx/scenedesc.hpp>

#include "commands.hpp"
#include "placing.hpp"
#include "skinpass.hpp"

namespace Rtx
{
    namespace
    {
        /// The rows both halves are built from, made before either: `makeInstanceRecords` fills a
        /// vector, and a member is initialised from a value.
        std::vector<InstanceRecord> recordsOf(const SceneDesc& scene)
        {
            std::vector<InstanceRecord> records;
            makeInstanceRecords(scene, records);
            return records;
        }
    }

    DeviceScene::DeviceScene(const Device& device, Graveyard& graveyard, Batch& batch, const SetLayout& textureLayout,
        const SkinPass& skin, const SceneDesc& scene, std::span<const TextureData> textures)
        : mSkin(skin)
        , mRecords(recordsOf(scene))
        , mAcceleration(device, graveyard, batch, scene, sFrameSlots)
        , mBuffers(device, graveyard, batch, scene, mRecords, sFrameSlots)
        , mSkinTables(device, graveyard, batch, scene, sFrameSlots)
        , mTextures(device, graveyard, batch, textureLayout,
              static_cast<std::uint32_t>(scene.textures().getPaths().size()), textures)
    {
        // Posed before it is built. The structures are built over the first copy of the
        // positions, and a skinned body's bind pose is not where the body is; the pass writes the
        // pose into that copy and the build then reads it. The other copy is owed the same pose and
        // takes it on the first placement that writes it.
        mSkin.record(batch.getCommands(), scene, FrameSlot{}, mSkinTables, mAcceleration.getPoses(),
            mBuffers.getNormals(), nullptr);
        mAcceleration.build(batch, scene, mRecords);
        mBuiltMeshes = scene.meshes().getRevision();
        mBuiltStructure = scene.getStructureRevision();

        // The first copy's set, which the first frame binds before any placement pays it.
        mTextures.sync(FrameSlot{});
    }

    void DeviceScene::extend(
        Batch& batch, const SceneDesc& scene, std::span<const TextureData> arrived, GpuTimer* const timer)
    {
        mTextures.write(batch, arrived);

        if (scene.meshes().getRevision() != mBuiltMeshes)
        {
            mBuffers.extend(batch, scene);
            mSkinTables.extend(batch, scene);
            mAcceleration.extend(batch, scene);

            // Posed before it is built, as the constructor does, into the first copy, which is what
            // the build reads — and only the meshes that arrived, over the rows `SkinTables::extend`
            // staged. `SkinPass::recordArrived` says why it may not be every mesh the copy owes.
            mSkin.recordArrived(batch.getCommands(), scene, FrameSlot{}, scene.meshes().getArrived(), mSkinTables,
                mAcceleration.getPoses(), mBuffers.getNormals());
            mAcceleration.buildArrived(batch, scene, timer);
            mBuiltMeshes = scene.meshes().getRevision();
        }

        mBuiltStructure = scene.getStructureRevision();
    }

    bool DeviceScene::place(const SceneDesc& scene, const Placing& placing)
    {
        // What the scene let go of, given back here: walking away from a ring frees its meshes and
        // nothing arrives to take them over until the next ring, so a frame that only places is the
        // one that must not hold their structures.
        mAcceleration.release(scene.meshes().getFreed());

        // Once, for the slots that changed, and both halves read it: a nine-by-nine exterior is
        // fifty thousand rows with a matrix inverse apiece, and a frame changes a hundred.
        updateInstanceRecords(scene, mRecords, mChangedRecords);

        // The descriptors this copy's set owes, now that nothing on the queue reads it.
        mTextures.sync(placing.mSlot);

        // The pose first, because the refit reads it. Every skinned body and morphed face this
        // copy owes is computed into it here, and the barrier the pass ends in is what the refit
        // and the trace wait on.
        const bool posed = mSkin.record(placing.mCommands, scene, placing.mSlot, mSkinTables, mAcceleration.getPoses(),
            mBuffers.getNormals(), placing.mTimer);

        const bool built = mAcceleration.place(scene, mRecords, mChangedRecords, placing);

        // Nothing to report, because nothing here is recorded: the tables are host-visible and the
        // submit that follows makes them visible. Only what a moving world changed — rebuilding all
        // of it is tens of milliseconds on a nine-by-nine region.
        mBuffers.place(scene, mRecords, mChangedRecords, placing);

        return posed || built;
    }

    void DeviceScene::finishReads(const FrameSlot slot) const
    {
        mBuffers.finishReads(slot);
        mAcceleration.finishReads(slot);
        mSkinTables.finishReads(slot);
        mTextures.finishReads(slot);
    }

    SceneHeld DeviceScene::describe() const
    {
        return SceneHeld{
            .mBuilt = true,
            .mStructureRevision = mBuiltStructure,
            .mTextureCount = mTextures.getCount(),
        };
    }

    void DeviceScene::readPlacedStats(SceneStats& stats) const
    {
        stats.mInstances = mAcceleration.getInstanceCounts();
        stats.mTableBytes = mBuffers.getBytes() + mSkinTables.getBytes();

        // Read every placement and not with the rest of the report, because a placement is
        // where the answer lands: the queries a build wrote are read some placements later, so a
        // pair read at the build would be the nought that stands between the question and its
        // answer. `BottomLevelStore::getCompactableBytes` says why it is not asked for sooner.
        stats.mCompactableBytes = mAcceleration.getCompactableBytes();
        stats.mCompactableNowBytes = mAcceleration.getCompactableNowBytes();
    }

    void DeviceScene::readStats(SceneStats& stats) const
    {
        readPlacedStats(stats);

        stats.mStructureBytes = mAcceleration.getStructureBytes();
        stats.mStructureLiveBytes = mAcceleration.getStructureLiveBytes();

        const TexturesHeld textures = mTextures.getHeld();
        stats.mTextureCount = textures.mCount;
        stats.mTextureBytes = textures.mBytes;
    }
}
