#include "bottomlevelstore.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <utility>

#include <osg/Vec3f>

#include <components/crashcatcher/crashnote.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/meshtable.hpp>
#include <components/rtx/result.hpp>
#include <components/rtx/scenedesc.hpp>

#include "buffer.hpp"
#include "bufferusage.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "gputimer.hpp"
#include "graveyard.hpp"
#include "memory.hpp"
#include "result.hpp"
#include "timeline.hpp"

namespace Rtx
{
    namespace
    {
        /// How much a placement copies tight before it leaves the rest to the next one. A budget
        /// and not the lot, because a copy needs the tight room while the loose room is still
        /// standing, so compacting a cell in one placement would make the high-water mark the sum.
        /// At this rate a cell is tight within a few dozen placements of arriving.
        constexpr VkDeviceSize sCompactionPerPlacement = 8 * 1024 * 1024;
    }

    BottomLevelStore::BottomLevelStore(const Device& device)
        : mDevice(device)
    {
    }

    void BottomLevelStore::retire(const Index slot)
    {
        Row& row = mRows[slot];

        Compaction& state = row.mCompaction;
        if (state.mTightness == Tightness::Answered)
        {
            mCompactableNow -= state.mBuiltSize;
            mCompactableTight -= state.mTightSize;
        }

        state.mTightness = Tightness::None;
        mDevice.getGraveyard().bury(std::move(row.mStructure));
    }

    void BottomLevelStore::release(std::span<const Index> meshes)
    {
        for (const Index mesh : meshes)
        {
            // A slot this never held: a scene can add a mesh and sweep it in the same window,
            // before anything was handed over to build it.
            if (mesh >= mRows.size())
                continue;

            retire(mesh);
        }
    }

    void BottomLevelStore::build(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes,
        const BlockedBuffer& poses, const BlockedBuffer& indices, const std::uint64_t placement,
        std::vector<Refusal>& refused)
    {
        const Crash::NoteScope noted("building bottom-level structures for {} meshes", meshes.size());
        const DeviceFunctions& functions = mDevice.getFunctions();
        const std::size_t held = scene.meshes().getRows().size();

        // Grown to what the scene now holds, and the scene never shrinks. Asserted and not guarded,
        // because a mesh table that shrank has no right answer: the `resize` below would drop the
        // handles above the new end and leak their structures.
        assert(held >= mRows.size() && "the scene's mesh table shrank under the structures");
        mRows.resize(held);

        mBuild.sizeTo(meshes.size());
        mLiveBuilds.clear();
        mLiveBuilds.reserve(meshes.size());

        const VkDeviceSize scratchAlignment = mDevice.getPhysicalDevice().getStructureScratchAlignment();

        // Sized before anything is created, so a load's structures land in one storage block rather
        // than one per mesh. An arrival asks for nothing and gets a block big enough for itself.
        VkDeviceSize wanted = 0;
        VkDeviceSize scratchTotal = 0;

        // Every row at nought, which is what a mesh with no triangles is left at: nothing
        // describes it, nothing builds it, and the gate below reads that nought.
        mBuilding.clear();
        mBuilding.resize(meshes.size());

        // A static mesh's vertices are a build input and nothing else, so they go with the submit:
        // a hit reads them back out of the structure through position fetch. Held for the life of
        // the cell they were a quarter of what a world reserved. A mesh that deforms is built over
        // the pose in the poses.
        VkDeviceSize arrivedBytes = 0;
        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            const MeshRange& mesh = scene.meshes().getRows()[meshes[at]];
            if (mesh.deforms() || mesh.mVertices.empty())
                continue;

            mBuilding[at].mArrivedAt = arrivedBytes;
            arrivedBytes += VkDeviceSize{ mesh.mVertices.mCount } * sizeof(osg::Vec3f);
        }

        // A byte where nothing static arrived, because a buffer of nothing cannot be created.
        growTo(mArrived, mDevice, BufferKind::DeviceLocal, arrivedBytes,
            sBuildInputUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "arrived positions");
        const VkDeviceAddress arrivedAddress = mArrived.addressFor();

        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            const Index mesh = meshes[at];
            const MeshRange& range = scene.meshes().getRows()[mesh];
            if (range.deforms() || range.mVertices.empty())
                continue;

            stageInto(batch, mArrived, mBuilding[at].mArrivedAt, std::as_bytes(scene.meshes().getMeshPositions(mesh)));
        }

        if (arrivedBytes > 0)
            orderStagedWrites(batch);

        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            const Index slot = meshes[at];
            const MeshRange& mesh = scene.meshes().getRows()[slot];
            Row& row = mRows[slot];

            // A slot handed out again arrives holding different geometry. Whatever was there is
            // buried and its room given back before this one asks for room of its own, so the
            // two can be the same run.
            retire(slot);

            // A pose or an arrival's staging, and which one is what the mesh is. A deforming
            // mesh is built over what `SkinPass` wrote into the first copy ahead of this, so its
            // structure carries the pose rather than the bind; a static one is built over the
            // vertices staged above.
            VkDeviceAddress vertices = 0;
            if (!mesh.mVertices.empty())
                vertices
                    = mesh.deforms() ? poses.addressOf(mesh.mBindOffset) : arrivedAddress + mBuilding[at].mArrivedAt;

            // Indices are mesh-local, so each structure is handed the slice of the shared buffers
            // that belongs to it and addresses vertex zero as its own first vertex. The addresses
            // are guarded here as well: a freed slot's run is nothing, and `addressOf` would name
            // where it used to be.
            mBuild.mGeometries[at] = describeTriangles(
                mesh, vertices, !mesh.mIndices.empty() ? indices.addressOf(mesh.mIndices.mOffset) : 0);

            // Only a mesh that deforms is built to be refitted. The flag costs a structure its
            // tightness and the trace that reads it a little; a few dozen actors pay it and the
            // thousands of static meshes around them do not.
            row.mUpdatable = mesh.deforms();

            // ALLOW_DATA_ACCESS is what lets a shader read a hit triangle's vertices back out of
            // the structure, which is the whole reason nothing here binds a vertex buffer.
            VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
            if (mesh.deforms())
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            else
            {
                // What lets the builder be asked what it would come to tight, for a fraction of a
                // per cent against the half it gives back. A mesh that refits is left out: a refit
                // writes back into the slack.
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            }

            mBuild.mBuilds[at] = VkAccelerationStructureBuildGeometryInfoKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = flags,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .geometryCount = 1,
                .pGeometries = &mBuild.mGeometries[at],
            };

            const std::uint32_t triangles = mesh.getTriangleCount();

            // A freed slot gets no structure at all. It keeps its index and its room and holds
            // nothing until something fits into it, and a build over no primitives is not a small
            // structure — it is a size the driver may answer zero for, which is not a size an
            // acceleration structure can be created at.
            if (triangles == 0)
            {
                row.mUpdateScratch = 0;
                continue;
            }

            VkAccelerationStructureBuildSizesInfoKHR sizes{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
            };
            functions.mGetAccelerationStructureBuildSizes(mDevice.getHandle(),
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mBuild.mBuilds[at], &triangles, &sizes);

            mBuilding[at].mSize = sizes.accelerationStructureSize;
            wanted = alignUp(wanted + sizes.accelerationStructureSize, StructureStorage::sAlignment);

            mBuilding[at].mScratchOffset = scratchTotal;
            scratchTotal = alignUp(scratchTotal + sizes.buildScratchSize, scratchAlignment);

            // Kept so a refit of this one mesh does not have to ask the driver its size again. The
            // same geometry describes it, so the answer cannot have changed. The build's own scratch
            // beside it, for the rota that builds a refitted structure whole again, which counts
            // from this build.
            row.mUpdateScratch = sizes.updateScratchSize;
            row.mBuildScratch = sizes.buildScratchSize;
            row.mRebuiltAt = placement;

            mBuild.mRanges[at] = VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = triangles };
        }

        if (scratchTotal == 0)
            return;

        growTo(mScratch, mDevice, BufferKind::DeviceLocal, scratchTotal, sScratchUsage, "build scratch");
        const VkDeviceAddress scratchAddress = mScratch.addressFor();

        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            if (mBuilding[at].mSize == 0)
                continue;

            const Index slot = meshes[at];
            Row& row = mRows[slot];

            // Left out where the device has no room: the slot holds no structure and so nothing a
            // refit or a rebuild reads, and the scratch it was counted into goes unread. A load's
            // whole total is not asked for again once the device has refused it, and each structure
            // after asks for its own room.
            const Result<StructureRoom, std::string_view> room = mStorage.take(mDevice, mBuilding[at].mSize, wanted);
            if (!room.isOk())
            {
                row.mUpdatable = false;
                row.mUpdateScratch = 0;
                row.mBuildScratch = 0;
                wanted = 0;
                refused.push_back(Refusal{ .mKind = Refused::Mesh, .mWhy = std::string(room.error()) });
                continue;
            }

            row.mStructure = AccelerationStructure::bottomLevel(mDevice, mStorage, room.value(), mBuilding[at].mSize);

            mBuild.mBuilds[at].dstAccelerationStructure = row.mStructure.getHandle();
            mBuild.mBuilds[at].scratchData.deviceAddress = scratchAddress + mBuilding[at].mScratchOffset;

            // Kept per slot so the figure compaction is judged against covers the whole scene rather
            // than the meshes this call happened to build.
            row.mCompaction.mBuiltSize = mBuilding[at].mSize;

            // Built loose whatever stood in the slot before, and a mesh that refits keeps its
            // slack: a refit writes back into it.
            row.mCompaction.mTightness = row.mUpdatable ? Tightness::None : Tightness::Loose;

            mLiveBuilds.push_back(mBuild.mBuilds[at]);
            mBuild.mRangePointers.push_back(&mBuild.mRanges[at]);
        }

        // Every structure refused: a build of none is not a command Vulkan takes.
        if (mLiveBuilds.empty())
            return;

        const VkCommandBuffer commands = batch.getCommands();
        functions.mCmdBuildAccelerationStructures(
            commands, static_cast<std::uint32_t>(mLiveBuilds.size()), mLiveBuilds.data(), mBuild.mRangePointers.data());
        barrierAfterBuild(commands);

        askWhatCompactionWouldSave(commands);
    }

    void BottomLevelStore::askWhatCompactionWouldSave(const VkCommandBuffer commands)
    {
        const auto held = static_cast<std::uint32_t>(mRows.size());
        if (held > mCompactablePool)
        {
            // Twice what it held, so a route's arrivals make a pool a logarithmic number of times
            // rather than one per cell.
            const std::uint32_t wanted = std::max(held, 2 * mCompactablePool);

            // The pool this replaces may be named by a batch the queue has not reached, and every
            // answer it was to carry is lost with it: whoever was asked through it is asked again
            // through the new one, below.
            const VkQueryPoolCreateInfo create{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                .queryCount = wanted,
            };
            mDevice.getGraveyard().replace(
                mCompactable, QueryPool::make(mDevice.getHandle(), vkCreateQueryPool, create, "vkCreateQueryPool"));
            mCompactablePool = wanted;

            for (Row& row : mRows)
                if (row.mCompaction.mTightness == Tightness::Asked)
                    row.mCompaction.mTightness = Tightness::Loose;
        }

        // One reset and one write per run of consecutive slots, which is what a cell's
        // arrivals are: the scene hands out its slots in order. Each query is reset before it is
        // written because the slot may have been asked about before, for a structure that has
        // since gone.
        //
        // The value the batch rides, which is the pool's next submit: a batch is flushed into
        // it or deferred ahead of it, and nothing else takes a value in between.
        const std::uint64_t rides = mDevice.getTimeline().getNext();
        std::uint32_t first = 0;
        mAskScratch.clear();
        for (std::uint32_t slot = 0; slot < held; ++slot)
        {
            Compaction& state = mRows[slot].mCompaction;
            if (state.mTightness != Tightness::Loose)
            {
                askRun(commands, first);
                continue;
            }

            if (mAskScratch.empty())
                first = slot;
            mAskScratch.push_back(mRows[slot].mStructure.getHandle());

            state.mTightness = Tightness::Asked;
            state.mAskedAt = rides;
            mAsked.push(Ask{ .mSlot = static_cast<Index>(slot), .mAt = rides });
        }
        askRun(commands, first);
    }

    void BottomLevelStore::askRun(const VkCommandBuffer commands, const std::uint32_t first)
    {
        if (mAskScratch.empty())
            return;

        const auto count = static_cast<std::uint32_t>(mAskScratch.size());
        vkCmdResetQueryPool(commands, mCompactable.get(), first, count);
        mDevice.getFunctions().mCmdWriteAccelerationStructuresProperties(commands, count, mAskScratch.data(),
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, mCompactable.get(), first);
        mAskScratch.clear();
    }

    void BottomLevelStore::readAnswers()
    {
        while (!mAsked.empty())
        {
            const Ask& oldest = mAsked.at(0);

            // Read once the host has waited past the submit that carried the question, so which
            // frame compacts a structure is a function of the frames and not of the clock. The
            // questions are in the order they were asked, so what is ready is a prefix.
            if (!mDevice.getTimeline().hasFinished(oldest.mAt))
                break;

            if (!isOutstanding(oldest))
            {
                mAsked.pop(1);
                continue;
            }

            // The run of consecutive slots asked together, read as one range. Cut where a slot was
            // asked again since, whose query a later batch may still be writing.
            std::size_t count = 1;
            while (count < mAsked.size() && mAsked.at(count).mAt == oldest.mAt
                && mAsked.at(count).mSlot == mAsked.at(count - 1).mSlot + 1 && isOutstanding(mAsked.at(count)))
                ++count;

            mReadScratch.resize(count);
            const VkResult read = vkGetQueryPoolResults(mDevice.getHandle(), mCompactable.get(), oldest.mSlot,
                static_cast<std::uint32_t>(count), count * sizeof(VkDeviceSize), mReadScratch.data(),
                sizeof(VkDeviceSize), VK_QUERY_RESULT_64_BIT);

            // Asked again next placement where the answers are simply not there yet. Anything else
            // is a fault, and a lost device shows up here first: read as "no saving", it was a
            // frame of tight structures and a message that came a submit later.
            if (read == VK_NOT_READY)
                break;

            checkVk(mDevice, read, "vkGetQueryPoolResults");

            for (std::size_t at = 0; at < count; ++at)
            {
                const Index slot = mAsked.at(at).mSlot;
                Compaction& state = mRows[slot].mCompaction;

                // A driver that says a tight copy would be no smaller leaves the structure as it
                // was built: the copy would spend a room and a command to change nothing.
                const VkDeviceSize tight = mReadScratch[at];
                if (tight == 0 || tight >= state.mBuiltSize)
                {
                    state.mTightness = Tightness::Tight;
                    continue;
                }

                state.mTightness = Tightness::Answered;
                state.mTightSize = tight;
                mCompactableNow += state.mBuiltSize;
                mCompactableTight += tight;
                mAnswered.push(slot);
            }

            mAsked.pop(count);
        }

        mAsked.settle();
    }

    const SlotSet& BottomLevelStore::prepareCompaction()
    {
        mCompactionCopies.clear();
        mMovedMeshes.clear();

        readAnswers();

        VkDeviceSize taken = 0;
        while (!mAnswered.empty() && taken < sCompactionPerPlacement)
        {
            const Index slot = mAnswered.at(0);

            // The slot may have been handed out again since it answered. A cell that left took
            // its meshes with it, and whatever stands here now is not what this answer is about —
            // its own question is.
            Row& row = mRows[slot];
            Compaction& state = row.mCompaction;
            if (state.mTightness != Tightness::Answered)
            {
                mAnswered.pop(1);
                continue;
            }

            // Made in a room of its own while the loose one stands, because the copy reads the
            // loose one; the top level can be built over the tight one in this same command
            // buffer, because its address is its own from the moment it is made. Taken before the
            // answer is popped: a device that refuses the room leaves the answer where it was, to
            // be asked again by a later placement, rather than a structure answered and never
            // copied.
            const VkDeviceSize tight = state.mTightSize;
            const Result<StructureRoom, std::string_view> room = mStorage.take(mDevice, tight, sCompactionPerPlacement);
            if (!room.isOk())
                break;

            AccelerationStructure made = AccelerationStructure::bottomLevel(mDevice, mStorage, room.value(), tight);
            mAnswered.pop(1);

            mCompactionCopies.push_back(VkCopyAccelerationStructureInfoKHR{
                .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                .src = row.mStructure.getHandle(),
                .dst = made.getHandle(),
                .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR,
            });

            // Buried and not destroyed, though the copy below reads it. The graveyard lets go
            // once the frame this is recorded into retires, and the copy runs inside that frame —
            // so what the fence covers is both this read and whatever earlier frame is still
            // tracing the structure through the top level it was named in.
            mDevice.getGraveyard().replace(row.mStructure, std::move(made));

            // The pair the report prints follows the copy, so what it says is what is left to save
            // rather than what was saved once.
            mCompactableNow -= state.mBuiltSize;
            mCompactableTight -= tight;

            state.mBuiltSize = tight;
            state.mTightness = Tightness::Tight;

            mMovedMeshes.addMakingRoom(slot);
            taken += tight;
        }

        mAnswered.settle();

        return mMovedMeshes;
    }

    void BottomLevelStore::recordCompaction(const VkCommandBuffer commands, GpuTimer* const timer)
    {
        openZone(timer, commands, "compact");

        const DeviceFunctions& functions = mDevice.getFunctions();
        for (const VkCopyAccelerationStructureInfoKHR& copy : mCompactionCopies)
            functions.mCmdCopyAccelerationStructure(commands, &copy);

        barrierAfterBuild(commands);
        closeZone(timer, commands);
    }

}
