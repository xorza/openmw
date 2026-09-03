#include "sceneacceleration.hpp"

#include <cassert>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/frametimes.hpp>
#include <components/rtx/scenedesc.hpp>
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

        // Addressable as well as build input, because the shader reads the indices back at a hit
        // through the same address the build read them at, and there is no reason for a second copy
        // of them to exist. No descriptor names any of these, so nothing else is asked for.
        constexpr VkBufferUsageFlags sBuildInputUsage
            = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        constexpr VkBufferUsageFlags sStorageUsage
            = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        constexpr VkBufferUsageFlags sScratchUsage
            = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        /// One mesh's triangles as the builder takes them, out of addresses a caller worked out.
        ///
        /// **`maxVertex` is guarded, because a freed slot has no vertices.** A slot the scene has
        /// taken back keeps its index and its room and holds a count of zero until something fits
        /// into it; subtracting one there wraps, and the driver is handed four billion vertices.
        ///
        /// **Opaque as built**, and overridden per instance where a material says otherwise: opacity
        /// is a property of the material and a mesh does not carry one, so the top-level flags are
        /// the only place the question can be answered exactly.
        ///
        /// @param micromap what the mesh's cutout mask baked to, chained so that traversal resolves
        ///        each microtriangle the bake decided without the any-hit — or null for a mesh with
        ///        none, whose forced non-opaque rows reach the any-hit for every candidate.
        VkAccelerationStructureGeometryKHR describeTriangles(const MeshRange& mesh, VkDeviceAddress positions,
            VkDeviceAddress indices, const VkAccelerationStructureTrianglesOpacityMicromapEXT* micromap)
        {
            return VkAccelerationStructureGeometryKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .geometry = { .triangles = {
                                  .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                  .pNext = micromap,
                                  .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                  .vertexData = { .deviceAddress = positions },
                                  .vertexStride = sizeof(osg::Vec3f),
                                  .maxVertex = mesh.mVertexCount > 0 ? mesh.mVertexCount - 1 : 0,
                                  .indexType = VK_INDEX_TYPE_UINT32,
                                  .indexData = { .deviceAddress = indices },
                              } },
                .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
            };
        }

        /// Everything between a build and whatever reads the structure it wrote.
        /// Orders a build after the trace before it on the queue, which may still be reading what
        /// the build is about to write.
        ///
        /// **What the fence used to be.** With one frame in flight the trace had finished before the
        /// next placement was recorded; with two it has not, and a top level or a refit built over
        /// a structure a ray is walking is a torn structure. An execution dependency is all a
        /// write-after-read needs.
        void barrierBeforeBuild(VkCommandBuffer commands)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
                    | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                .srcAccessMask
                = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                .dstAccessMask
                = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            };
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(commands, &dependency);
        }

        void barrierAfterBuild(VkCommandBuffer commands)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                    | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
            };
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(commands, &dependency);
        }
    }

    VkTransformMatrixKHR toVulkanTransform(const Transform3x4& transform)
    {
        VkTransformMatrixKHR result{};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                result.matrix[row][column] = transform.mRows[row][column];

        return result;
    }

    SceneAcceleration::SceneAcceleration(const Device& device, const SceneDesc& scene, const std::uint32_t slots)
        : mDevice(device)
        , mSlots(slots)
    {
        assert(slots >= 1 && slots <= sFrameSlots && "more frames in flight than there are copies of the rows");

        mPositions.open(device, slots, sBuildInputUsage, "positions");
        mRowTable.open(device, slots, sBuildInputUsage, "instances");
        mIndices.open(device, sBuildInputUsage, "indices");

        // Every mesh the scene holds, which is the same path an arrival takes with a shorter list.
        mEveryMesh.resize(scene.getMeshes().size());
        for (std::size_t at = 0; at < mEveryMesh.size(); ++at)
            mEveryMesh[at] = static_cast<Index>(at);

        writeGeometry(scene, mEveryMesh);

        // Every copy of the positions holds what it will ever read from here, so what a copy owes
        // from now on is the poses it missed.
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
            mPositions.settle(slot);
    }

    void SceneAcceleration::build(Batch& batch, const SceneDesc& scene, std::span<const InstanceRecord> records,
        const SceneMicromaps& micromaps, Graveyard& graveyard)
    {
        assert(mBottomLevel.empty() && mTopLevel == VK_NULL_HANDLE && "a scene built twice");

        // The rows after the structures, because a row names the address of the structure it places.
        buildMeshes(batch, scene, mEveryMesh, micromaps, graveyard);
        writeRows(records, {});
        prepareTopLevel(scene, 0, graveyard);
        recordTopLevel(batch.getCommands(), nullptr);
    }

    SceneAcceleration::~SceneAcceleration()
    {
        const DeviceFunctions& functions = mDevice.getFunctions();

        if (mTopLevel != VK_NULL_HANDLE)
            functions.mDestroyAccelerationStructure(mDevice.getHandle(), mTopLevel, nullptr);

        for (const VkAccelerationStructureKHR structure : mBottomLevel)
            if (structure != VK_NULL_HANDLE)
                functions.mDestroyAccelerationStructure(mDevice.getHandle(), structure, nullptr);
    }

    void SceneAcceleration::writeGeometry(const SceneDesc& scene, std::span<const Index> meshes)
    {
        // The scene's own reach, so a block exists for every run it has handed out. Blocks already
        // made are left exactly where they are, and one call reaches every copy — `SlotBlocks` is
        // what holds one per frame in flight.
        mPositions.reserve(static_cast<std::uint32_t>(scene.getPositions().size()));
        mIndices.reserve(static_cast<std::uint32_t>(scene.getIndices().size()));

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.getMeshes()[mesh];
            if (range.mVertexCount == 0)
                continue;

            // The first copy is what a structure is built from; a mesh that deforms is refitted from
            // whichever copy its frame owns, so its bind pose goes into every one until the pass
            // writes a pose over it.
            const std::span<const osg::Vec3f> positions
                = scene.getPositions().subspan(range.mVertexOffset, range.mVertexCount);
            mPositions.at(0).writeAt(range.mVertexOffset, positions);
            if (range.mDeform != Deform::None)
                for (std::uint32_t slot = 1; slot < mSlots; ++slot)
                    mPositions.at(slot).writeAt(range.mVertexOffset, positions);

            mIndices.writeAt(range.mIndexOffset, scene.getIndices().subspan(range.mIndexOffset, range.mIndexCount));
        }
    }

    void SceneAcceleration::release(std::span<const Index> meshes, Graveyard& graveyard)
    {
        for (const Index mesh : meshes)
        {
            // A slot this never held: a scene can add a mesh and sweep it in the same window,
            // before anything was handed over to build it.
            if (mesh >= mBottomLevel.size())
                continue;

            graveyard.bury(mBottomLevel[mesh]);
            graveyard.bury(mBottomLevelStorage, mBottomLevelRooms[mesh]);

            mBottomLevel[mesh] = VK_NULL_HANDLE;
            mBottomLevelAddresses[mesh] = 0;
            mBottomLevelRooms[mesh] = StructureRoom{};
            mMicromapped[mesh] = 0;
        }
    }

    void SceneAcceleration::extend(const SceneDesc& scene, Graveyard& graveyard)
    {
        // **Departures first, and their rooms go to the graveyard rather than straight back**, so an
        // arrival this frame cannot be built into room a frame in flight is still tracing. The two
        // lists are disjoint, so a slot handed out again appears only among the arrivals and is
        // dealt with by `buildMeshes`, which buries whatever the slot was holding.
        release(scene.getFreedMeshes(), graveyard);

        writeGeometry(scene, scene.getArrivedMeshes());
    }

    void SceneAcceleration::buildArrived(
        Batch& batch, const SceneDesc& scene, const SceneMicromaps& micromaps, GpuTimer* timer, Graveyard& graveyard)
    {
        // **The builds a crossing brings, bracketed as one zone.** Without it they are device time
        // the frame's fence carries and no zone accounts for, so the frame a player feels is the one
        // frame whose report says nothing about what made it slow.
        openZone(timer, batch.getCommands(), "blas");

        const auto opened = std::chrono::steady_clock::now();
        buildMeshes(batch, scene, scene.getArrivedMeshes(), micromaps, graveyard);

        Log(Debug::Verbose) << "  scene build: " << since(opened, std::chrono::steady_clock::now())
                            << " ms recording structures for " << scene.getArrivedMeshes().size() << " meshes";

        closeZone(timer, batch.getCommands());
    }

    void SceneAcceleration::buildMeshes(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes,
        const SceneMicromaps& micromaps, Graveyard& graveyard)
    {
        const DeviceFunctions& functions = mDevice.getFunctions();
        const std::size_t slots = scene.getMeshes().size();

        // Grown to what the scene now holds, never shrunk: a slot the scene took back keeps its
        // index, and the tables below are indexed by it.
        mBottomLevel.resize(slots, VK_NULL_HANDLE);
        mBottomLevelAddresses.resize(slots, 0);
        mBottomLevelRooms.resize(slots);
        mUpdateScratch.resize(slots, 0);
        mUpdatable.resize(slots, 0);
        mMicromapped.resize(slots, 0);

        // The build reads these through pointers it keeps until the command is recorded, so they
        // live across the whole function rather than inside the loop.
        mBuildGeometries.assign(meshes.size(), VkAccelerationStructureGeometryKHR{});
        mBuildMicromaps.assign(meshes.size(), VkAccelerationStructureTrianglesOpacityMicromapEXT{});
        mBuilds.assign(meshes.size(), VkAccelerationStructureBuildGeometryInfoKHR{});
        mBuildRanges.assign(meshes.size(), VkAccelerationStructureBuildRangeInfoKHR{});
        mBuildRangePointers.clear();
        mLiveBuilds.clear();
        mBuildRangePointers.reserve(meshes.size());
        mLiveBuilds.reserve(meshes.size());

        const VkDeviceSize scratchAlignment
            = mDevice.getPhysicalDevice()
                  .getProperties()
                  .mAccelerationStructure.minAccelerationStructureScratchOffsetAlignment;

        // Sized before anything is created, so a load's structures land in one storage block rather
        // than one per mesh. An arrival asks for nothing and gets a block big enough for itself.
        VkDeviceSize wanted = 0;
        VkDeviceSize scratchTotal = 0;

        mBuildScratchOffsets.clear();
        mBuildScratchOffsets.resize(meshes.size());

        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            const Index slot = meshes[at];
            const MeshRange& mesh = scene.getMeshes()[slot];

            // **A slot handed out again arrives holding different geometry.** Whatever was there is
            // destroyed and its room given back before this one asks for room of its own, so the
            // two can be the same run.
            if (mBottomLevel[slot] != VK_NULL_HANDLE)
            {
                graveyard.bury(mBottomLevel[slot]);
                graveyard.bury(mBottomLevelStorage, mBottomLevelRooms[slot]);
                mBottomLevel[slot] = VK_NULL_HANDLE;
                mBottomLevelAddresses[slot] = 0;
                mBottomLevelRooms[slot] = StructureRoom{};
            }

            // **Over its micromap, where the bake gave it one.** The structure is what carries the
            // micromap from here on: a refit has to describe the same one, and a row placing the
            // mesh counts by it.
            mMicromapped[slot] = micromaps.has(slot) ? 1 : 0;
            if (mMicromapped[slot] != 0)
                mBuildMicromaps[at] = micromaps.describe(slot);

            // Indices are mesh-local, so each structure is handed the slice of the shared buffers
            // that belongs to it and addresses vertex zero as its own first vertex. The addresses
            // are guarded here as well: a freed slot's run is nothing, and `addressOf` would name
            // where it used to be.
            mBuildGeometries[at]
                = describeTriangles(mesh, mesh.mVertexCount > 0 ? mPositions.at(0).addressOf(mesh.mVertexOffset) : 0,
                    mesh.mIndexCount > 0 ? mIndices.addressOf(mesh.mIndexOffset) : 0,
                    mMicromapped[slot] != 0 ? &mBuildMicromaps[at] : nullptr);

            // **Only a mesh that deforms is built to be refitted.** The flag costs a structure its
            // tightness and the trace that reads it a little; a few dozen actors pay it and the
            // thousands of static meshes around them do not.
            mUpdatable[slot] = mesh.mDeform != Deform::None ? 1 : 0;

            // ALLOW_DATA_ACCESS is what lets a shader read a hit triangle's vertices back out of
            // the structure, which is the whole reason nothing here binds a vertex buffer.
            VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
            if (mesh.mDeform != Deform::None)
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;

            mBuilds[at] = VkAccelerationStructureBuildGeometryInfoKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = flags,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .geometryCount = 1,
                .pGeometries = &mBuildGeometries[at],
            };

            const std::uint32_t triangles = mesh.getTriangleCount();

            // **A freed slot gets no structure at all.** It keeps its index and its room and holds
            // nothing until something fits into it, and a build over no primitives is not a small
            // structure — it is a size the driver may answer zero for, which is not a size an
            // acceleration structure can be created at.
            if (triangles == 0)
            {
                mUpdateScratch[slot] = 0;
                mBuildSizes.resize(meshes.size());
                mBuildSizes[at] = 0;
                continue;
            }

            VkAccelerationStructureBuildSizesInfoKHR sizes{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
            };
            functions.mGetAccelerationStructureBuildSizes(
                mDevice.getHandle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mBuilds[at], &triangles, &sizes);

            mBuildSizes.resize(meshes.size());
            mBuildSizes[at] = sizes.accelerationStructureSize;
            wanted = alignUp(wanted + sizes.accelerationStructureSize, StructureStorage::sAlignment);

            mBuildScratchOffsets[at] = scratchTotal;
            scratchTotal = alignUp(scratchTotal + sizes.buildScratchSize, scratchAlignment);

            // Kept so a refit of this one mesh does not have to ask the driver its size again. The
            // same geometry describes it, so the answer cannot have changed.
            mUpdateScratch[slot] = sizes.updateScratchSize;

            mBuildRanges[at] = VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = triangles };
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
            mBottomLevelRooms[slot] = mBottomLevelStorage.take(mDevice, mBuildSizes[at], wanted);

            const VkAccelerationStructureCreateInfoKHR create{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                .buffer = mBottomLevelStorage.getBuffer(mBottomLevelRooms[slot]),
                .offset = mBottomLevelStorage.getOffset(mBottomLevelRooms[slot]),
                .size = mBuildSizes[at],
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            };
            checkVk(functions.mCreateAccelerationStructure(mDevice.getHandle(), &create, nullptr, &mBottomLevel[slot]),
                "vkCreateAccelerationStructureKHR");

            mBuilds[at].dstAccelerationStructure = mBottomLevel[slot];
            mBuilds[at].scratchData.deviceAddress = scratchAddress + mBuildScratchOffsets[at];

            // **Asked once each, here, and never again.** A handle lasts until the mesh is released
            // and its address with it, so the alternative is the same question per instance per
            // frame — fifty thousand driver round trips on a nine-by-nine exterior for fifty
            // thousand answers that cannot have changed.
            const VkAccelerationStructureDeviceAddressInfoKHR address{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = mBottomLevel[slot],
            };
            mBottomLevelAddresses[slot]
                = functions.mGetAccelerationStructureDeviceAddress(mDevice.getHandle(), &address);

            mLiveBuilds.push_back(mBuilds[at]);
            mBuildRangePointers.push_back(&mBuildRanges[at]);
        }

        const VkCommandBuffer commands = batch.getCommands();
        functions.mCmdBuildAccelerationStructures(
            commands, static_cast<std::uint32_t>(mLiveBuilds.size()), mLiveBuilds.data(), mBuildRangePointers.data());
        barrierAfterBuild(commands);

        batch.keep(std::move(scratch));
    }

    void SceneAcceleration::prepareRefit(
        const SceneDesc& scene, const std::uint32_t slot, const SceneMicromaps& micromaps, Graveyard& graveyard)
    {
        const std::span<const Index> deformed = scene.getDeformed();

        // **This frame's copy, which the pass has already posed into.** `SkinPass::record` runs
        // ahead of this in the same command buffer and pays the positions' account — every pose this
        // copy owed, this frame's and the ones it missed — so what the refit reads is the pose and
        // not the bind.
        BlockedBuffer& positions = mPositions.at(slot);

        if (deformed.empty())
        {
            // **Emptied and not left alone.** These still hold the last frame's rebuilds, and a
            // frame whose actors have all gone would otherwise leave a vector whose size claims work
            // that is not there.
            mRefitBuilds.clear();
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
            assert(mUpdatable[mesh] != 0 && "a mesh posed that was not built to be refitted");
            scratchTotal = alignUp(scratchTotal + mUpdateScratch[mesh], scratchAlignment);
        }

        if (mRefitScratch.getSize() < scratchTotal)
            graveyard.bury(std::exchange(mRefitScratch, Buffer::deviceLocal(mDevice, scratchTotal, sScratchUsage)));

        const VkDeviceAddress scratchAddress = mRefitScratch.getDeviceAddress();

        mRefitGeometries.resize(count);
        mRefitMicromaps.resize(count);
        mRefitBuilds.resize(count);
        mRefitRanges.resize(count);
        mRefitRangePointers.resize(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            const Index index = deformed[i];
            const MeshRange& mesh = scene.getMeshes()[index];

            // The same description the first build was given, micromap included, which is what
            // makes the structure it produces the same size as the one already sitting at this
            // mesh's offset — and what an update over a micromap requires.
            if (mMicromapped[index] != 0)
                mRefitMicromaps[i] = micromaps.describe(index);

            mRefitGeometries[i] = describeTriangles(mesh, positions.addressOf(mesh.mVertexOffset),
                mIndices.addressOf(mesh.mIndexOffset), mMicromapped[index] != 0 ? &mRefitMicromaps[i] : nullptr);

            mRefitRanges[i] = VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = mesh.getTriangleCount() };
            mRefitRangePointers[i] = &mRefitRanges[i];
        }

        // A second pass, because `pGeometries` is a pointer into a vector the first pass was still
        // filling: a build info written beside a geometry that later moved would name freed memory.
        VkDeviceSize scratchAt = 0;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const Index index = deformed[i];

            // **Into the structure that is already there**, rather than into a new one beside it:
            // its handle is what every top-level row already points at. An update, with the same
            // flags as the build that allowed one, which the update requires.
            mRefitBuilds[i] = VkAccelerationStructureBuildGeometryInfoKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                    | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR
                    | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,
                .srcAccelerationStructure = mBottomLevel[index],
                .dstAccelerationStructure = mBottomLevel[index],
                .geometryCount = 1,
                .pGeometries = &mRefitGeometries[i],
                .scratchData = { .deviceAddress = scratchAddress + scratchAt },
            };

            scratchAt = alignUp(scratchAt + mUpdateScratch[index], scratchAlignment);
        }
    }

    void SceneAcceleration::recordRefit(VkCommandBuffer commands, GpuTimer* timer)
    {
        openZone(timer, commands, "refit");
        mDevice.getFunctions().mCmdBuildAccelerationStructures(
            commands, static_cast<std::uint32_t>(mRefitBuilds.size()), mRefitBuilds.data(), mRefitRangePointers.data());
        barrierAfterBuild(commands);
        closeZone(timer, commands);
    }

    bool SceneAcceleration::place(VkCommandBuffer commands, const SceneDesc& scene,
        std::span<const InstanceRecord> records, std::span<const Index> changed, const std::uint32_t slot,
        const SceneMicromaps& micromaps, GpuTimer* timer, Graveyard& graveyard)
    {
        assert(slot < mSlots && "a frame slot this scene has no copy of the rows for");

        prepareRefit(scene, slot, micromaps, graveyard);

        // **What this copy owes, and not what the scene moved.** The top level is built from this
        // copy of the rows, so what decides whether it has to be built again is whether those rows
        // are about to change — a debt this copy may have carried for frames, not a list the current
        // frame filled. A world that stands still owes nothing and still returns here, which is what
        // the early return is for: building the same top level over the same rows was a submit and a
        // fence on every frame of a standing camera. A refit alone still rebuilds it, because a top
        // level caches the bounds of what it names.
        writeRows(records, changed);
        if (!mRowTable.owes(slot) && mRefitBuilds.empty())
            return false;

        prepareTopLevel(scene, slot, graveyard);

        // The barrier between the refit and the top level is what the fence used to be: the top
        // level is built over structures the refit has just rewritten, which is a dependency inside
        // a command buffer rather than a reason to go round the driver twice.
        barrierBeforeBuild(commands);
        if (!mRefitBuilds.empty())
            recordRefit(commands, timer);

        recordTopLevel(commands, timer);
        return true;
    }

    void SceneAcceleration::writeRows(std::span<const InstanceRecord> records, std::span<const Index> changed)
    {
        const std::size_t had = mRowTable.size();
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

    void SceneAcceleration::prepareTopLevel(const SceneDesc& scene, const std::uint32_t slot, Graveyard& graveyard)
    {
        // **Checked here rather than left to the driver.** A scene that grew a mesh since `setScene`
        // built the structures is a caller breaking `placeScene`'s contract, and the only symptom is
        // a top level naming a bottom level that was never made — which surfaces as an invalid handle
        // inside `vkGetAccelerationStructureDeviceAddressKHR` and says nothing about who did it. One
        // comparison, once a frame, for a failure that otherwise takes the process down unexplained.
        if (scene.getMeshes().size() != mBottomLevel.size())
            throw Error("the scene grew from " + std::to_string(mBottomLevel.size()) + " meshes to "
                + std::to_string(scene.getMeshes().size())
                + " without being built again; placeScene can only move what setScene made");

        mRowTable.sync(slot, graveyard);

        const auto count = static_cast<std::uint32_t>(mRowTable.size());
        if (mTopLevel == VK_NULL_HANDLE || count > mTopLevelSlots)
            sizeTopLevel(count, graveyard);

        // The top level is built from this frame's copy, so the address moves with the slot.
        mTopLevelGeometry.geometry.instances.data.deviceAddress = mRowTable.getDeviceAddress(slot);

        mInstanceCount = scene.getPlacedCount();
    }

    void SceneAcceleration::placeRow(const Index slot, const InstanceRecord& record)
    {
        std::uint8_t& counted = mRowFlags[slot];
        if ((counted & sRowCutout) != 0)
            --mCutoutInstanceCount;
        if ((counted & sRowMicromapped) != 0)
            --mMicromappedInstanceCount;
        if ((counted & sRowWater) != 0)
            --mWaterInstanceCount;
        counted = 0;

        // **A gap is an inactive row and not a row left out.** Its slot is the custom index a hit
        // reads back, so the rows cannot close up around it; a reference of nought is what the
        // build reads as an instance to skip, and it costs the build nothing it would ever trace.
        if (!record.mPlaced)
        {
            mRowTable.write(slot) = VkAccelerationStructureInstanceKHR{};
            return;
        }

        if (record.mMask == Shaders::MASK_WATER)
        {
            counted |= sRowWater;
            ++mWaterInstanceCount;
        }

        // Morrowind's sheet geometry is lit and hit from both faces, so nothing is culled.
        VkGeometryInstanceFlagsKHR flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;

        assert(record.mMesh < mMicromapped.size() && "a row placing a mesh nothing built");
        const bool micromapped = record.mCutout && !record.mTranslucent && mMicromapped[record.mMesh] != 0;

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
            ++mCutoutInstanceCount;
        }

        if (micromapped)
        {
            counted |= sRowMicromapped;
            ++mMicromappedInstanceCount;
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
            .accelerationStructureReference = mBottomLevelAddresses[record.mMesh],
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
