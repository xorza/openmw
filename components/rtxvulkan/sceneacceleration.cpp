#include "sceneacceleration.hpp"

#include <cassert>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include <components/rtx/error.hpp>
#include <components/rtx/scenetables.hpp>
#include <components/rtx/shaders/scene.h>

#include "commands.hpp"
#include "device.hpp"
#include "gputimer.hpp"
#include "graveyard.hpp"
#include "result.hpp"
#include "scenemicromaps.hpp"

namespace Rtx
{
    namespace
    {
        /// What a row counts as, kept beside it so the counts move with the row.
        constexpr std::uint8_t sRowCutout = 1;
        constexpr std::uint8_t sRowWater = 2;
        constexpr std::uint8_t sRowMicromapped = 4;
        constexpr std::uint8_t sRowMedium = 8;

        constexpr VkBufferUsageFlags sStorageUsage
            = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    VkTransformMatrixKHR toVulkanTransform(const Transform3x4& transform)
    {
        VkTransformMatrixKHR result{};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                result.matrix[row][column] = transform.mRows[row][column];

        return result;
    }

    SceneAcceleration::SceneAcceleration(
        const Device& device, Batch& batch, const SceneTables& scene, const std::uint32_t slots)
        : mDevice(device)
        , mSlots(slots)
        , mBottomLevel(device, slots)
    {
        assert(slots >= 1 && slots <= sFrameSlots && "more frames in flight than there are copies of the rows");

        mPoses.open(device, slots, sBuildInputUsage, "poses");
        mRowTable.open(device, slots, sBuildInputUsage, "instances");
        mIndices.open(device, sBuildInputUsage, "indices");

        // Every mesh the scene holds, which is the same path an arrival takes with a shorter list.
        mEveryMesh.resize(scene.mMeshes.getRows().size());
        for (std::size_t at = 0; at < mEveryMesh.size(); ++at)
            mEveryMesh[at] = static_cast<Index>(at);

        writeGeometry(batch, scene, mEveryMesh);

        // Every copy holds a bind pose for every body the scene arrived with, so what a copy owes
        // from now on is the poses it missed.
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
            mPoses.settle(FrameSlot{ slot });
    }

    void SceneAcceleration::build(Batch& batch, const SceneTables& scene, std::span<const InstanceRecord> records,
        const SceneMicromaps& micromaps, Graveyard& graveyard)
    {
        assert(mBottomLevel.size() == 0 && mTopLevel == VK_NULL_HANDLE && "a scene built twice");

        // The rows after the structures, because a row names the address of the structure it places.
        mBottomLevel.build(batch, scene, mEveryMesh, micromaps, mPoses.at(FrameSlot{}), mIndices, graveyard);
        writeRows(records, {});
        prepareTopLevel(scene, FrameSlot{}, graveyard);
        recordTopLevel(batch.getCommands(), nullptr);
    }

    SceneAcceleration::~SceneAcceleration()
    {
        if (mTopLevel != VK_NULL_HANDLE)
            mDevice.getFunctions().mDestroyAccelerationStructure(mDevice.getHandle(), mTopLevel, nullptr);
    }

    void SceneAcceleration::writeGeometry(Batch& batch, const SceneTables& scene, std::span<const Index> meshes)
    {
        // Each table's own reach, so a block exists for every run it has handed out. Blocks already
        // made are left exactly where they are, and one call reaches every copy — `SlotBlocks` is
        // what holds one per frame in flight.
        mPoses.reserve(batch, scene.mDeformers.getBindVertexCount());
        mIndices.reserve(batch, static_cast<std::uint32_t>(scene.mMeshes.getIndices().size()));

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.mMeshes.getRows()[mesh];
            if (range.mVertices.empty())
                continue;

            // **The bind pose into every copy, and only for a mesh that has one.** A body stands in
            // whatever pose the copy being traced was last given, so a copy the pass has never
            // dispatched for it still has to hold something a refit can read. A static mesh has no
            // run here at all: `buildMeshes` stages its vertices for the build and nothing else.
            if (range.mDeform != Deform::None)
                for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                    mPoses.at(FrameSlot{ slot })
                        .writeAt(batch, range.mBindOffset, scene.mMeshes.getMeshPositions(mesh));

            mIndices.writeAt(batch, range.mIndices.mOffset, range.mIndices.in(scene.mMeshes.getIndices()));
        }

        // **What is built out of these was copied a moment ago.** The blocks are device memory, so a
        // mesh reaches them through a transfer rather than through a host write that a submit already
        // orders — and the acceleration structures built from them are recorded into this same
        // command buffer. One dependency for every block, because they are read together.
        orderStagedWrites(batch);
    }

    void SceneAcceleration::extend(Batch& batch, const SceneTables& scene, Graveyard& graveyard)
    {
        // **Departures first, and their rooms go to the graveyard rather than straight back**, so an
        // arrival this frame cannot be built into room a frame in flight is still tracing. The two
        // lists are disjoint, so a slot handed out again appears only among the arrivals and is
        // dealt with by `buildMeshes`, which buries whatever the slot was holding.
        release(scene.mMeshes.getFreed(), graveyard);

        writeGeometry(batch, scene, scene.mMeshes.getArrived());
    }

    void SceneAcceleration::buildArrived(
        Batch& batch, const SceneTables& scene, const SceneMicromaps& micromaps, GpuTimer* timer, Graveyard& graveyard)
    {
        // **The builds a crossing brings, bracketed as one zone.** Without it they are device time
        // the frame's fence carries and no zone accounts for, so the frame a player feels is the one
        // frame whose report says nothing about what made it slow.
        openZone(timer, batch.getCommands(), "blas");

        mBottomLevel.build(
            batch, scene, scene.mMeshes.getArrived(), micromaps, mPoses.at(FrameSlot{}), mIndices, graveyard);

        closeZone(timer, batch.getCommands());
    }

    void SceneAcceleration::prepareRefit(
        const SceneTables& scene, const FrameSlot slot, const SceneMicromaps& micromaps, Graveyard& graveyard)
    {
        const std::span<const Index> deformed = scene.mMeshes.getDeformed();

        // **This frame's copy, which the pass has already posed into.** `SkinPass::record` runs
        // ahead of this in the same command buffer and pays the poses' account — every pose this
        // copy owed, this frame's and the ones it missed — so what the refit reads is the pose and
        // not the bind.
        BlockedBuffer& poses = mPoses.at(slot);

        if (deformed.empty())
        {
            // **Emptied and not left alone.** These still hold the last frame's rebuilds, and a
            // frame whose actors have all gone would otherwise leave a vector whose size claims work
            // that is not there.
            mRefit.sizeTo(0);
            return;
        }

        const auto count = static_cast<std::uint32_t>(deformed.size());

        const VkDeviceSize scratchAlignment
            = mDevice.getPhysicalDevice()
                  .getProperties()
                  .mAccelerationStructure.minAccelerationStructureScratchOffsetAlignment;

        VkDeviceSize scratchTotal = 0;
        for (const Index mesh : deformed)
        {
            assert(mesh < mBottomLevel.size() && "a mesh this holds no structure for");
            assert(mBottomLevel.isUpdatable(mesh) && "a mesh posed that was not built to be refitted");
            scratchTotal = alignUp(scratchTotal + mBottomLevel.getUpdateScratch(mesh), scratchAlignment);
        }

        if (mRefitScratch.getSize() < scratchTotal)
            graveyard.bury(std::exchange(mRefitScratch, Buffer::deviceLocal(mDevice, scratchTotal, sScratchUsage)));

        const VkDeviceAddress scratchAddress = mRefitScratch.getDeviceAddress();

        mRefit.sizeTo(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            const Index index = deformed[i];
            const MeshRange& mesh = scene.mMeshes.getRows()[index];

            // The same description the first build was given, micromap included, which is what
            // makes the structure it produces the same size as the one already sitting at this
            // mesh's offset — and what an update over a micromap requires.
            const bool micromapped = mBottomLevel.isMicromapped(index);
            if (micromapped)
                mRefit.mMicromaps[i] = micromaps.describe(index);

            mRefit.mGeometries[i] = describeTriangles(mesh, poses.addressOf(mesh.mBindOffset),
                mIndices.addressOf(mesh.mIndices.mOffset), micromapped ? &mRefit.mMicromaps[i] : nullptr);

            mRefit.mRanges[i] = VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = mesh.getTriangleCount() };
            mRefit.mRangePointers.push_back(&mRefit.mRanges[i]);
        }

        // A second pass, for the reason `StructureBuildBatch` gives: the geometries are placed
        // before any build info names one.
        VkDeviceSize scratchAt = 0;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const Index index = deformed[i];

            // **Into the structure that is already there**, rather than into a new one beside it:
            // its handle is what every top-level row already points at. An update, with the same
            // flags as the build that allowed one, which the update requires.
            mRefit.mBuilds[i] = VkAccelerationStructureBuildGeometryInfoKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                    | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR
                    | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,
                .srcAccelerationStructure = mBottomLevel.getStructure(index),
                .dstAccelerationStructure = mBottomLevel.getStructure(index),
                .geometryCount = 1,
                .pGeometries = &mRefit.mGeometries[i],
                .scratchData = { .deviceAddress = scratchAddress + scratchAt },
            };

            scratchAt = alignUp(scratchAt + mBottomLevel.getUpdateScratch(index), scratchAlignment);
        }
    }

    void SceneAcceleration::recordRefit(VkCommandBuffer commands, GpuTimer* timer)
    {
        openZone(timer, commands, "refit");
        mDevice.getFunctions().mCmdBuildAccelerationStructures(commands,
            static_cast<std::uint32_t>(mRefit.mBuilds.size()), mRefit.mBuilds.data(), mRefit.mRangePointers.data());
        barrierAfterBuild(commands);
        closeZone(timer, commands);
    }

    bool SceneAcceleration::place(const SceneTables& scene, std::span<const InstanceRecord> records,
        std::span<const Index> changed, const SceneMicromaps& micromaps, const Placing& placing)
    {
        assert(placing.mSlot.get() < mSlots && "a frame slot this scene has no copy of the rows for");

        prepareRefit(scene, placing.mSlot, micromaps, placing.mGraveyard);

        // **What this copy owes, and not what the scene moved.** The top level is built from this
        // copy of the rows, so what decides whether it has to be built again is whether those rows
        // are about to change — a debt this copy may have carried for frames, not a list the current
        // frame filled. A world that stands still owes nothing and still returns here, which is what
        // the early return is for: building the same top level over the same rows was a submit and a
        // fence on every frame of a standing camera. A refit alone still rebuilds it, because a top
        // level caches the bounds of what it names.
        writeRows(records, changed);

        // **After the rows are grown to the scene and before the copy they are synced from.** A
        // structure copied tight has moved, and the rows naming it are written again here — into
        // the same table, so every copy owes them the way it owes anything else.
        const bool compacting = placeCompacted(records, placing.mGraveyard);

        if (!compacting && !mRowTable.owes(placing.mSlot) && mRefit.mBuilds.empty())
            return false;

        prepareTopLevel(scene, placing.mSlot, placing.mGraveyard);

        // The barrier between the refit and the top level is what the fence used to be: the top
        // level is built over structures the refit has just rewritten, which is a dependency inside
        // a command buffer rather than a reason to go round the driver twice.
        barrierBeforeBuild(placing.mCommands);
        if (compacting)
            mBottomLevel.recordCompaction(placing.mCommands, placing.mTimer);

        if (!mRefit.mBuilds.empty())
            recordRefit(placing.mCommands, placing.mTimer);

        recordTopLevel(placing.mCommands, placing.mTimer);
        return true;
    }

    bool SceneAcceleration::placeCompacted(std::span<const InstanceRecord> records, Graveyard& graveyard)
    {
        const SlotSet& moved = mBottomLevel.prepareCompaction(graveyard);
        if (moved.empty())
            return false;

        // **Every row that placed one of these names an address that has moved.** Nothing indexes
        // the instances by the mesh they place, so the records are walked — only on a placement that
        // compacted something, which is the twenty or so after a cell arrives and never again for
        // those meshes.
        for (std::size_t at = 0; at < records.size(); ++at)
        {
            const InstanceRecord& record = records[at];
            if (record.mPlaced && moved.has(record.mMesh))
                placeRow(static_cast<Index>(at), record);
        }

        return true;
    }

    void SceneAcceleration::writeRows(std::span<const InstanceRecord> records, std::span<const Index> changed)
    {
        const std::size_t had = mRowTable.size();

        // **A row that leaves discounts itself before its flags go.** `mRowFlags` is what says what
        // a row counted as, and the resize below drops the flags of the rows past the new end — so
        // a cutout or a medium that left with them would stay in the totals for the rest of the
        // scene.
        assert(mRowFlags.size() == had && "the row flags and the rows fell out of step");
        for (std::size_t at = records.size(); at < had; ++at)
            discountRow(static_cast<Index>(at));

        mRowTable.resize(records.size());
        mRowFlags.resize(records.size(), 0);

        // **What the table grew by, written from its record rather than left inactive.** `resize`
        // owes every appended row to every copy, so a row nothing writes reaches the device as a
        // gap rather than as whatever was last in that memory. This is what makes them the
        // instances they actually are, and on the first placement it is the whole table.
        for (std::size_t at = had; at < records.size(); ++at)
            placeRow(static_cast<Index>(at), records[at]);

        for (const Index at : changed)
            placeRow(at, records[at]);
    }

    void SceneAcceleration::prepareTopLevel(const SceneTables& scene, const FrameSlot slot, Graveyard& graveyard)
    {
        // **Checked here rather than left to the driver.** A scene that grew a mesh since `setScene`
        // built the structures is a caller breaking `placeScene`'s contract, and the only symptom is
        // a top level naming a bottom level that was never made — which surfaces as an invalid handle
        // inside `vkGetAccelerationStructureDeviceAddressKHR` and says nothing about who did it. One
        // comparison, once a frame, for a failure that otherwise takes the process down unexplained.
        if (scene.mMeshes.getRows().size() != mBottomLevel.size())
            throw Error("the scene grew from " + std::to_string(mBottomLevel.size()) + " meshes to "
                + std::to_string(scene.mMeshes.getRows().size())
                + " without being built again; placeScene can only move what setScene made");

        mRowTable.sync(slot, graveyard);

        const auto count = static_cast<std::uint32_t>(mRowTable.size());
        if (mTopLevel == VK_NULL_HANDLE || count > mTopLevelSlots)
            sizeTopLevel(count, graveyard);

        // The top level is built from this frame's copy, so the address moves with the slot.
        mTopLevelGeometry.geometry.instances.data.deviceAddress = mRowTable.getDeviceAddress(slot);

        mCounts.mPlaced = scene.mPlacements.getPlacedCount();
    }

    void SceneAcceleration::discountRow(const Index slot)
    {
        std::uint8_t& counted = mRowFlags[slot];
        if ((counted & sRowCutout) != 0)
            --mCounts.mCutout;
        if ((counted & sRowMicromapped) != 0)
            --mCounts.mMicromapped;
        if ((counted & sRowWater) != 0)
            --mCounts.mWater;
        if ((counted & sRowMedium) != 0)
            --mCounts.mMedium;
        counted = 0;
    }

    void SceneAcceleration::placeRow(const Index slot, const InstanceRecord& record)
    {
        discountRow(slot);

        // **A gap is an inactive row and not a row left out.** Its slot is the custom index a hit
        // reads back, so the rows cannot close up around it; a reference of nought is what the
        // build reads as an instance to skip, and it costs the build nothing it would ever trace.
        if (!record.mPlaced)
        {
            mRowTable.write(slot) = VkAccelerationStructureInstanceKHR{};
            return;
        }

        std::uint8_t& counted = mRowFlags[slot];

        // **A test on the bit and not on the whole mask.** A row carries `MASK_MEDIUM` beside
        // whichever of the three it is, so an equality here would stop counting the day anything
        // that is water is also a medium.
        if ((record.mMask & Shaders::MASK_WATER) != 0)
        {
            counted |= sRowWater;
            ++mCounts.mWater;
        }

        if ((record.mMask & Shaders::MASK_MEDIUM) != 0)
        {
            counted |= sRowMedium;
            ++mCounts.mMedium;
        }

        // Morrowind's sheet geometry is lit and hit from both faces, so nothing is culled.
        VkGeometryInstanceFlagsKHR flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;

        assert(record.mMesh < mBottomLevel.size() && "a row placing a mesh nothing built");
        const bool micromapped = record.mCutout && !record.mTranslucent && mBottomLevel.isMicromapped(record.mMesh);

        // **The geometry is built opaque, so forcing is the whole of how either candidate reaches
        // the shader at all** — a cutout to be asked whether there is anything at the hit, a
        // translucent surface to be asked how much of it there is.
        //
        // **Except over a micromap, whose answer the forced bit would override.** The lookup
        // replaces the geometry's own opaque bit and the instance's flags are applied after it, so
        // a row forced non-opaque sends every leaf to the any-hit and culls only the holes —
        // measured, and `aMicromapAnswersAtTheFinestLevel...` is what says so. Left alone, the
        // micromap decides: a hole is ignored, a leaf commits, and only an unknown microtriangle
        // reaches the shader.
        //
        // **And that override is exactly what a placement the game is fading wants.** A leaf that
        // committed without asking would stop a shadow ray that dims by the fade and walks on
        // today, so its row is forced non-opaque like any translucent one: every leaf reaches the
        // any-hit, a hole is still a hole, and the micromap stays on the structure for the frame
        // the fade ends. Not `DISABLE_OPACITY_MICROMAPS`, which asks a structure built to allow
        // it and lost the device on a crossing over one that was not.
        if ((record.mCutout && !micromapped) || record.mTranslucent)
            flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;

        // A translucent instance is never asked the cutout's question, so it is not counted against
        // the cutout's cost however its material is marked.
        if (record.mCutout && !record.mTranslucent)
        {
            counted |= sRowCutout;
            ++mCounts.mCutout;
        }

        if (micromapped)
        {
            counted |= sRowMicromapped;
            ++mCounts.mMicromapped;
        }

        mRowTable.write(slot) = VkAccelerationStructureInstanceKHR{
            .transform = toVulkanTransform(record.mTransform),
            // A row's position is the custom index the shader reads back at a hit.
            .instanceCustomIndex = slot & 0xFFFFFFu,
            .mask = record.mMask,

            // **The kind, so that traversal picks the shader and the trace never asks what it
            // hit.** One closest-hit shader stands at each record of the visibility pass's hit
            // table, in the order `MaterialKind` names them, and the reorder's first key is the
            // shader that record names. Written here rather than read from a material row at the
            // hit, which is the read this replaces.
            .instanceShaderBindingTableRecordOffset = static_cast<std::uint32_t>(record.mKind),
            .flags = flags,
            .accelerationStructureReference = mBottomLevel.getAddress(record.mMesh),
        };
    }

    void SceneAcceleration::sizeTopLevel(const std::uint32_t slots, Graveyard& graveyard)
    {
        const DeviceFunctions& functions = mDevice.getFunctions();

        // The address is the caller's to fill in, because it is a frame's and not the structure's.
        mTopLevelGeometry = VkAccelerationStructureGeometryKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry = { .instances = {
                              .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                          } },
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        };

        mTopLevelBuild = VkAccelerationStructureBuildGeometryInfoKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1,
            .pGeometries = &mTopLevelGeometry,
        };

        VkAccelerationStructureBuildSizesInfoKHR sizes{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
        };
        functions.mGetAccelerationStructureBuildSizes(
            mDevice.getHandle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mTopLevelBuild, &slots, &sizes);

        // **The old structure is buried, and its storage with it where that has to grow.** A cell
        // arriving is what brings this here, and an arrival waits every frame out first — but the
        // rule is one rule, and burying costs nothing where nothing is in flight.
        graveyard.bury(mTopLevel);
        mTopLevel = VK_NULL_HANDLE;

        mTopLevelBytes = sizes.accelerationStructureSize;
        mTopLevelSlots = slots;

        // Grown to the high-water mark and kept, both of them. A structure is created at offset zero
        // of whatever this holds and asks only that it be large enough.
        if (mTopLevelStorage.getSize() < sizes.accelerationStructureSize)
            graveyard.bury(std::exchange(
                mTopLevelStorage, Buffer::deviceLocal(mDevice, sizes.accelerationStructureSize, sStorageUsage)));

        if (mTopLevelScratch.getSize() < sizes.buildScratchSize)
            graveyard.bury(
                std::exchange(mTopLevelScratch, Buffer::deviceLocal(mDevice, sizes.buildScratchSize, sScratchUsage)));

        const VkAccelerationStructureCreateInfoKHR create{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = mTopLevelStorage.getHandle(),
            .size = sizes.accelerationStructureSize,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        };
        checkVk(functions.mCreateAccelerationStructure(mDevice.getHandle(), &create, nullptr, &mTopLevel),
            "vkCreateAccelerationStructureKHR");
        mDevice.setName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<std::uint64_t>(mTopLevel), "scene");

        mTopLevelBuild.dstAccelerationStructure = mTopLevel;
        mTopLevelBuild.scratchData.deviceAddress = mTopLevelScratch.getDeviceAddress();
    }

    void SceneAcceleration::recordTopLevel(VkCommandBuffer commands, GpuTimer* timer)
    {
        const VkAccelerationStructureBuildRangeInfoKHR range{ .primitiveCount = mTopLevelSlots };
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

        openZone(timer, commands, "tlas");
        mDevice.getFunctions().mCmdBuildAccelerationStructures(commands, 1, &mTopLevelBuild, &ranges);
        barrierAfterBuild(commands);
        closeZone(timer, commands);
    }
}
