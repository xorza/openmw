#include "bottomlevelstore.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

#include <osg/Vec3f>

#include <components/rtx/meshrange.hpp>
#include <components/rtx/meshtable.hpp>
#include <components/rtx/scenetables.hpp>

#include "buffer.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "gputimer.hpp"
#include "graveyard.hpp"
#include "memory.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// How much a placement copies tight before it leaves the rest to the next one.
        ///
        /// **A budget and not the lot, because a copy needs the tight room while the loose room is
        /// still standing.** Compacting a cell in one placement would ask the storage for the whole
        /// saving on top of what it was saving, and give the old rooms back only once the frame
        /// retired — so the high-water mark would be the sum rather than the difference, and the
        /// frame that did it would carry the whole copy. At this rate Seyda Neen's 133 MiB are
        /// tight within twenty placements of arriving, and what is outstanding at any moment is a
        /// block rather than a cell.
        constexpr VkDeviceSize sCompactionPerPlacement = 8 * 1024 * 1024;
    }

    BottomLevelStore::BottomLevelStore(const Device& device, const std::uint32_t slots)
        : mDevice(device)
        , mSlots(slots)
    {
    }

    BottomLevelStore::~BottomLevelStore()
    {
        const DeviceFunctions& functions = mDevice.getFunctions();
        for (const VkAccelerationStructureKHR structure : mStructures)
            if (structure != VK_NULL_HANDLE)
                functions.mDestroyAccelerationStructure(mDevice.getHandle(), structure, nullptr);
    }

    void BottomLevelStore::release(std::span<const Index> meshes, Graveyard& graveyard)
    {
        for (const Index mesh : meshes)
        {
            // A slot this never held: a scene can add a mesh and sweep it in the same window,
            // before anything was handed over to build it.
            if (mesh >= mStructures.size())
                continue;

            graveyard.bury(mStructures[mesh]);
            graveyard.bury(mStorage, mRooms[mesh]);

            mStructures[mesh] = VK_NULL_HANDLE;
            mAddresses[mesh] = 0;
            mRooms[mesh] = StructureRoom{};
        }
    }

    void BottomLevelStore::build(Batch& batch, const SceneTables& scene, std::span<const Index> meshes,
        const BlockedBuffer& poses, const BlockedBuffer& indices, Graveyard& graveyard)
    {
        const DeviceFunctions& functions = mDevice.getFunctions();
        const std::size_t held = scene.mMeshes.getRows().size();

        // Grown to what the scene now holds, and the scene never shrinks: a slot it took back keeps
        // its index, and the tables below are indexed by it. **Asserted and not guarded**, because
        // a mesh table that shrank has no right answer — the `resize` below drops the handles above
        // the new end and leaks their structures, and a guard keeps structures for meshes that are
        // gone.
        assert(held >= mStructures.size() && "the scene's mesh table shrank under the structures");
        mStructures.resize(held, VK_NULL_HANDLE);
        mAddresses.resize(held, 0);
        mRooms.resize(held);
        mUpdateScratch.resize(held, 0);
        mUpdatable.resize(held, 0);
        mBuiltSize.resize(held, 0);
        mCompacted.resize(held, 0);

        mBuild.sizeTo(meshes.size());
        mLiveBuilds.clear();
        mLiveBuilds.reserve(meshes.size());

        const VkDeviceSize scratchAlignment
            = mDevice.getPhysicalDevice()
                  .getProperties()
                  .mAccelerationStructure.minAccelerationStructureScratchOffsetAlignment;

        // Sized before anything is created, so a load's structures land in one storage block rather
        // than one per mesh. An arrival asks for nothing and gets a block big enough for itself.
        VkDeviceSize wanted = 0;
        VkDeviceSize scratchTotal = 0;

        // **Sized together and every entry at nought**, which is what a mesh with no triangles is
        // left at: nothing describes it, nothing builds it, and the gate below reads that nought.
        mBuildSizes.clear();
        mBuildSizes.resize(meshes.size());
        mBuildScratchOffsets.clear();
        mBuildScratchOffsets.resize(meshes.size());

        // **A static mesh's vertices are a build input and nothing else, so they go with the
        // submit.** A hit reads its triangle's vertices back out of the structure through position
        // fetch and it is never refitted, so the builder is the last thing that ever looks at them.
        // Held in a table for the life of the cell they were the whole scene's vertices standing
        // for one read apiece — a quarter of what a world reserved. A mesh that deforms is not
        // here: it is built over the pose in the poses, which is its own destination every frame.
        mArrivedAt.clear();
        mArrivedAt.resize(meshes.size());

        VkDeviceSize arrivedBytes = 0;
        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            const MeshRange& mesh = scene.mMeshes.getRows()[meshes[at]];
            if (mesh.mDeform != Deform::None || mesh.mVertices.empty())
                continue;

            mArrivedAt[at] = arrivedBytes;
            arrivedBytes += VkDeviceSize{ mesh.mVertices.mCount } * sizeof(osg::Vec3f);
        }

        // A byte where nothing static arrived, because a buffer of nothing cannot be created. The
        // address outlives the move: it belongs to the handle, which the batch now holds until its
        // submit has run — the same keeping the build's own scratch gets below.
        Buffer arrived = Buffer::deviceLocal(
            mDevice, std::max(arrivedBytes, VkDeviceSize{ 1 }), sBuildInputUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        mDevice.setName(
            VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(arrived.getHandle()), "arrived positions");
        const VkDeviceAddress arrivedAddress = arrived.getDeviceAddress();

        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            const Index mesh = meshes[at];
            const MeshRange& range = scene.mMeshes.getRows()[mesh];
            if (range.mDeform != Deform::None || range.mVertices.empty())
                continue;

            stageInto(batch, mDevice, arrived, mArrivedAt[at], std::as_bytes(scene.mMeshes.getMeshPositions(mesh)));
        }

        batch.keep(std::move(arrived));

        if (arrivedBytes > 0)
            orderStagedWrites(batch);

        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            const Index slot = meshes[at];
            const MeshRange& mesh = scene.mMeshes.getRows()[slot];

            // **A slot handed out again arrives holding different geometry.** Whatever was there is
            // destroyed and its room given back before this one asks for room of its own, so the
            // two can be the same run.
            if (mStructures[slot] != VK_NULL_HANDLE)
            {
                graveyard.bury(mStructures[slot]);
                graveyard.bury(mStorage, mRooms[slot]);
                mStructures[slot] = VK_NULL_HANDLE;
                mAddresses[slot] = 0;
                mRooms[slot] = StructureRoom{};
            }

            // **A pose or an arrival's staging, and which one is what the mesh is.** A deforming
            // mesh is built over what `SkinPass` wrote into the first copy ahead of this, so its
            // structure carries the pose rather than the bind; a static one is built over the
            // vertices staged above.
            VkDeviceAddress vertices = 0;
            if (!mesh.mVertices.empty())
                vertices = mesh.mDeform != Deform::None ? poses.addressOf(mesh.mBindOffset)
                                                        : arrivedAddress + mArrivedAt[at];

            // Indices are mesh-local, so each structure is handed the slice of the shared buffers
            // that belongs to it and addresses vertex zero as its own first vertex. The addresses
            // are guarded here as well: a freed slot's run is nothing, and `addressOf` would name
            // where it used to be.
            mBuild.mGeometries[at] = describeTriangles(
                mesh, vertices, !mesh.mIndices.empty() ? indices.addressOf(mesh.mIndices.mOffset) : 0);

            // **Only a mesh that deforms is built to be refitted.** The flag costs a structure its
            // tightness and the trace that reads it a little; a few dozen actors pay it and the
            // thousands of static meshes around them do not.
            mUpdatable[slot] = mesh.mDeform != Deform::None ? 1 : 0;

            // What is built here is built loose, whatever stood in the slot before was.
            mCompacted[slot] = 0;

            // ALLOW_DATA_ACCESS is what lets a shader read a hit triangle's vertices back out of
            // the structure, which is the whole reason nothing here binds a vertex buffer.
            VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
            if (mesh.mDeform != Deform::None)
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            else
            {
                // **What lets the builder be asked what it would come to tight.** A structure is
                // built loose because the builder cannot know the answer until it has finished, and
                // this is what makes the answer askable — measured at 0.6% of the structures for the
                // question, against the 60% `askWhatCompactionWouldSave` reports it would give back.
                // A mesh that refits is left out: a refit writes back into the slack.
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

            // **A freed slot gets no structure at all.** It keeps its index and its room and holds
            // nothing until something fits into it, and a build over no primitives is not a small
            // structure — it is a size the driver may answer zero for, which is not a size an
            // acceleration structure can be created at.
            if (triangles == 0)
            {
                mUpdateScratch[slot] = 0;
                continue;
            }

            VkAccelerationStructureBuildSizesInfoKHR sizes{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
            };
            functions.mGetAccelerationStructureBuildSizes(mDevice.getHandle(),
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mBuild.mBuilds[at], &triangles, &sizes);

            mBuildSizes[at] = sizes.accelerationStructureSize;
            wanted = alignUp(wanted + sizes.accelerationStructureSize, StructureStorage::sAlignment);

            mBuildScratchOffsets[at] = scratchTotal;
            scratchTotal = alignUp(scratchTotal + sizes.buildScratchSize, scratchAlignment);

            // Kept so a refit of this one mesh does not have to ask the driver its size again. The
            // same geometry describes it, so the answer cannot have changed.
            mUpdateScratch[slot] = sizes.updateScratchSize;

            mBuild.mRanges[at] = VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = triangles };
        }

        if (scratchTotal == 0)
            return;

        // Scratch is transient: it is read and written by the build and never again. It is handed to
        // the batch below rather than left to this scope, because the build it feeds has only been
        // recorded when this function returns — and the batch frees it the moment the flush does.
        Buffer scratch = Buffer::deviceLocal(mDevice, scratchTotal, sScratchUsage);
        const VkDeviceAddress scratchAddress = scratch.getDeviceAddress();

        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            if (mBuildSizes[at] == 0)
                continue;

            const Index slot = meshes[at];
            mRooms[slot] = mStorage.take(mDevice, mBuildSizes[at], wanted);

            const VkAccelerationStructureCreateInfoKHR create{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                .buffer = mStorage.getBuffer(mRooms[slot]),
                .offset = mStorage.getOffset(mRooms[slot]),
                .size = mBuildSizes[at],
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            };
            checkVk(functions.mCreateAccelerationStructure(mDevice.getHandle(), &create, nullptr, &mStructures[slot]),
                "vkCreateAccelerationStructureKHR");

            mBuild.mBuilds[at].dstAccelerationStructure = mStructures[slot];
            mBuild.mBuilds[at].scratchData.deviceAddress = scratchAddress + mBuildScratchOffsets[at];

            // **Asked once each, here, and never again.** A handle lasts until the mesh is released
            // and its address with it, so the alternative is the same question per instance per
            // frame — fifty thousand driver round trips on a nine-by-nine exterior for fifty
            // thousand answers that cannot have changed.
            const VkAccelerationStructureDeviceAddressInfoKHR address{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = mStructures[slot],
            };
            mAddresses[slot] = functions.mGetAccelerationStructureDeviceAddress(mDevice.getHandle(), &address);

            // Kept per slot so the figure compaction is judged against covers the whole scene rather
            // than the meshes this call happened to build.
            mBuiltSize[slot] = mBuildSizes[at];

            mLiveBuilds.push_back(mBuild.mBuilds[at]);
            mBuild.mRangePointers.push_back(&mBuild.mRanges[at]);
        }

        const VkCommandBuffer commands = batch.getCommands();
        functions.mCmdBuildAccelerationStructures(
            commands, static_cast<std::uint32_t>(mLiveBuilds.size()), mLiveBuilds.data(), mBuild.mRangePointers.data());
        barrierAfterBuild(commands);

        askWhatCompactionWouldSave(commands);

        batch.keep(std::move(scratch));
    }

    void BottomLevelStore::askWhatCompactionWouldSave(const VkCommandBuffer commands)
    {
        // **Every structure the scene holds, and not the ones this build made.** A route builds at
        // every crossing, so a figure about the last build is a figure about whatever the last
        // crossing happened to bring — nought, where it brought only actors. Asking about a
        // structure built earlier costs the query and nothing else.
        //
        // Only the ones built to allow it, which is every one that does not refit: a mesh that
        // deforms keeps its slack, because a refit writes back into it. And only the ones still
        // loose: a structure already copied tight would answer with its own size and be copied
        // again for nothing, so what the pair reports is what is left to save rather than what was
        // saved once.
        // **What is outstanding is dropped, not carried.** The handles and slots below are refilled,
        // so an answer from the round before would be read against another structure's slot — and a
        // tight size that belongs to a different mesh is a destination too small for the copy. What
        // that round had not reached is still loose, so this question asks about it again.
        mCompactedSizes.clear();
        mCompactionAt = 0;

        mCompactableHandles.clear();
        mCompactableSlots.clear();
        mCompactableNow = 0;
        mCompactableTight = 0;
        for (std::size_t slot = 0; slot < mStructures.size(); ++slot)
        {
            if (mStructures[slot] == VK_NULL_HANDLE || mUpdatable[slot] != 0 || mCompacted[slot] != 0)
                continue;

            mCompactableHandles.push_back(mStructures[slot]);
            mCompactableSlots.push_back(static_cast<Index>(slot));
            mCompactableNow += mBuiltSize[slot];
        }

        mCompactableCount = 0;
        mQueriedAt = sNoPlacement;

        if (mCompactableHandles.empty())
            return;

        const auto wanted = static_cast<std::uint32_t>(mCompactableHandles.size());
        if (wanted > mCompactablePool)
        {
            // **Destroyed rather than buried, because nothing is in flight here.** Both callers
            // drain the frames before they build — `setScene` waits the device idle and
            // `extendScene` finishes the ring — so a pool this replaces is named by no command
            // buffer the queue has yet to reach.

            const VkQueryPoolCreateInfo create{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                .queryCount = wanted,
            };
            checkVk(vkCreateQueryPool(mDevice.getHandle(), &create, nullptr, mCompactable.put(mDevice.getHandle())),
                "vkCreateQueryPool");

            mCompactablePool = wanted;
        }

        vkCmdResetQueryPool(commands, mCompactable.get(), 0, mCompactablePool);
        mDevice.getFunctions().mCmdWriteAccelerationStructuresProperties(commands, wanted, mCompactableHandles.data(),
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, mCompactable.get(), 0);

        mCompactableCount = wanted;

        // What says the answers are readable. `prepareCompaction` gives the rule.
        mQueriedAt = mPlacements;
    }

    const SlotSet& BottomLevelStore::prepareCompaction(Graveyard& graveyard)
    {
        ++mPlacements;

        mCompactionCopies.clear();
        mMovedMeshes.clear();

        // **Read once the placement that recorded the questions has certainly run.** The ring waits
        // for the frame `mSlots` back before it records this one, so a placement one further behind
        // than the submit that carried the questions has finished on the queue. That is the fence
        // this would otherwise have to keep, and `WAIT_BIT` in its place would stall the frame a
        // cell arrives in — the one frame that can least afford it.
        if (mQueriedAt != sNoPlacement && mPlacements > mQueriedAt + mSlots)
        {
            assert(mCompactableCount > 0 && "a question outstanding with no queries in it");

            mCompactedSizes.resize(mCompactableCount);
            const VkResult read = vkGetQueryPoolResults(mDevice.getHandle(), mCompactable.get(), 0, mCompactableCount,
                mCompactedSizes.size() * sizeof(VkDeviceSize), mCompactedSizes.data(), sizeof(VkDeviceSize),
                VK_QUERY_RESULT_64_BIT);

            // Asked again next placement where the answers are simply not there yet. A driver that
            // refuses outright leaves its structures as they were built, and the next build asks.
            if (read == VK_NOT_READY)
                mCompactedSizes.clear();
            else
            {
                mQueriedAt = sNoPlacement;
                mCompactionAt = 0;
                if (read != VK_SUCCESS)
                    mCompactedSizes.clear();

                // Summed where the answers arrive, because this is the only place they are read.
                // `getCompactableBytes` says what asking a second time would cost.
                VkDeviceSize tight = 0;
                for (const VkDeviceSize size : mCompactedSizes)
                    tight += size;

                mCompactableTight = tight;
            }
        }

        const DeviceFunctions& functions = mDevice.getFunctions();

        VkDeviceSize taken = 0;
        while (mCompactionAt < mCompactedSizes.size() && taken < sCompactionPerPlacement)
        {
            const std::size_t at = mCompactionAt++;
            const Index slot = mCompactableSlots[at];
            const VkDeviceSize tight = mCompactedSizes[at];

            // **The slot may have been handed out again since the question was asked.** A cell that
            // left took its meshes with it, and whatever stands here now is not what this answer is
            // about — the next build asks about that one.
            if (mStructures[slot] != mCompactableHandles[at])
                continue;

            // A structure the driver says is no smaller stays where it was built, and is not asked
            // about again: the copy would spend a room and a command to change nothing.
            if (tight == 0 || tight >= mBuiltSize[slot])
            {
                mCompacted[slot] = 1;
                continue;
            }

            const StructureRoom room = mStorage.take(mDevice, tight, sCompactionPerPlacement);
            const VkAccelerationStructureCreateInfoKHR create{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                .buffer = mStorage.getBuffer(room),
                .offset = mStorage.getOffset(room),
                .size = tight,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            };

            VkAccelerationStructureKHR made = VK_NULL_HANDLE;
            checkVk(functions.mCreateAccelerationStructure(mDevice.getHandle(), &create, nullptr, &made),
                "vkCreateAccelerationStructureKHR");

            mCompactionCopies.push_back(VkCopyAccelerationStructureInfoKHR{
                .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                .src = mStructures[slot],
                .dst = made,
                .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR,
            });

            // **Buried and not destroyed, though the copy below reads it.** The graveyard lets go
            // once the frame this is recorded into retires, and the copy runs inside that frame —
            // so what the fence covers is both this read and whatever earlier frame is still
            // tracing the structure through the top level it was named in.
            graveyard.bury(mStructures[slot]);
            graveyard.bury(mStorage, mRooms[slot]);

            // The pair the report prints follows the copy, so what it says is what is left to save
            // rather than what was saved once.
            mCompactableNow -= mBuiltSize[slot];
            mCompactableNow += tight;

            mStructures[slot] = made;
            mRooms[slot] = room;
            mBuiltSize[slot] = tight;
            mCompacted[slot] = 1;

            // Asked before the copy has run, which is what makes the top level buildable in this
            // same command buffer: an address belongs to the structure from the moment it is
            // created, and what the barrier orders is the contents arriving.
            const VkAccelerationStructureDeviceAddressInfoKHR address{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = made,
            };
            mAddresses[slot] = functions.mGetAccelerationStructureDeviceAddress(mDevice.getHandle(), &address);

            mMovedMeshes.addMakingRoom(slot);
            taken += tight;
        }

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
