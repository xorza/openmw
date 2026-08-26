#include "sceneacceleration.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include <components/rtx/alphabounds.hpp>
#include <components/rtx/alphaimage.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/micromap.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>

#include "commands.hpp"
#include "device.hpp"
#include "gputimer.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// Brackets a build where there is a timer to bracket it with.
        ///
        /// **The load path has none.** Building every structure from scratch is a cell arriving and
        /// not a frame, and its cost is already reported as a build time; giving it zones would put
        /// them in whichever frame report came next.
        void openZone(GpuTimer* timer, VkCommandBuffer commands, std::string_view name)
        {
            if (timer != nullptr)
                timer->open(commands, name);
        }

        void closeZone(GpuTimer* timer, VkCommandBuffer commands)
        {
            if (timer != nullptr)
                timer->close(commands);
        }

        /// `VkAccelerationStructureCreateInfoKHR::offset` must be a multiple of this.
        constexpr VkDeviceSize sStructureAlignment = 256;

        VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        // Storage as well as build input, because the shader reads the indices back at a hit and
        // there is no reason for a second copy of them to exist.
        constexpr VkBufferUsageFlags sBuildInputUsage
            = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        constexpr VkBufferUsageFlags sStorageUsage
            = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        constexpr VkBufferUsageFlags sScratchUsage
            = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        /// A micromap build reads its states and its triangle table from addresses Vulkan requires
        /// to be multiples of 256 — the same figure a structure's offset takes, for the same reason.
        constexpr VkDeviceSize sMicromapInputAlignment = 256;

        constexpr VkBufferUsageFlags sMicromapInputUsage
            = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        /// How far into a buffer the first 256-aligned address is.
        ///
        /// **Because a buffer's own address is not promised to be aligned to anything in
        /// particular.** Every driver in practice hands back a page, and a build that depended on
        /// that would be correct here and invalid on the next one.
        VkDeviceSize padTo(VkDeviceAddress address, VkDeviceSize alignment)
        {
            return (alignment - address % alignment) % alignment;
        }

        /// Everything between a micromap build and the structure build that references it.
        void barrierAfterMicromaps(VkCommandBuffer commands)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
                .srcAccessMask = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT,
            };
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(commands, &dependency);
        }

        /// A mesh more than one material stands on, which is a mesh no micromap can speak for.
        ///
        /// One below "no material at all", and no more a slot the scene can hand out than that is.
        constexpr Index sManyMaterials = sNoIndex - 1;

        /// Which material every mesh is worn by, or `sManyMaterials` where its placements disagree.
        void materialOfEachMesh(const SceneDesc& scene, std::vector<Index>& into)
        {
            into.assign(scene.getMeshes().size(), sNoIndex);

            for (const MeshInstance& instance : scene.getInstances())
            {
                if (!instance.isPlaced() || instance.mMesh >= into.size())
                    continue;

                Index& held = into[instance.mMesh];
                if (held == sNoIndex)
                    held = instance.mMaterial;
                else if (held != instance.mMaterial)
                    held = sManyMaterials;
            }
        }

        /// One texture slot among what a scene handed over, or nothing where it did not arrive.
        const TextureData* textureAt(std::span<const TextureData> textures, Index slot)
        {
            for (const TextureData& texture : textures)
                if (texture.mSlot == slot)
                    return &texture;

            return nullptr;
        }

        /// One mesh's classified micromap, waiting for the room and the command that build it.
        struct PendingMicromap
        {
            Index mMesh = sNoIndex;
            Micromap mMicromap;
        };

        /// A mask decoded and bounded once, against the one cutoff it was bounded for.
        ///
        /// A cell's cutout materials share a handful of images between them — 28 masks over 1580
        /// meshes at Seyda Neen — and a bound is a summed-area table over the finest level, so
        /// building one per mesh would decode the same canopy twenty times.
        struct BoundedMask
        {
            Index mTexture = sNoIndex;
            float mCutoff = 0.0f;
            AlphaBounds mBounds;
        };

        /// Everything between a build and whatever reads the structure it wrote.
        void barrierAfterBuild(VkCommandBuffer commands)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                .dstStageMask
                = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
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

    SceneAcceleration::SceneAcceleration(const Device& device, Batch& batch, const SceneDesc& scene,
        std::span<const InstanceRecord> records, std::span<const TextureData> textures)
        : mDevice(device)
    {
        assert(scene.getPlacedCount() > 0);

        mPositions.open(device, sBuildInputUsage, "positions");
        mIndices.open(device, sBuildInputUsage, "indices");

        // Every mesh the scene holds, which is the same path an arrival takes with a shorter list.
        mEveryMesh.resize(scene.getMeshes().size());
        for (std::size_t at = 0; at < mEveryMesh.size(); ++at)
            mEveryMesh[at] = static_cast<Index>(at);

        writeGeometry(scene, mEveryMesh);

        // **The geometry, every micromap, every bottom level and the top level in one submit.** Each
        // was its own round trip; the host writes above are visible to the submit without a barrier,
        // and each stage ends in the barrier the next one needs.
        buildMicromaps(batch, scene, textures, mEveryMesh);
        buildMeshes(batch, scene, mEveryMesh);
        prepareTopLevel(scene, records);
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

        for (const VkMicromapEXT micromap : mMicromaps)
            if (micromap != VK_NULL_HANDLE)
                functions.mDestroyMicromap(mDevice.getHandle(), micromap, nullptr);
    }

    void SceneAcceleration::writeGeometry(const SceneDesc& scene, std::span<const Index> meshes)
    {
        // The scene's own reach, so a block exists for every run it has handed out. Blocks already
        // made are left exactly where they are.
        mPositions.reserve(static_cast<std::uint32_t>(scene.getPositions().size()));
        mIndices.reserve(static_cast<std::uint32_t>(scene.getIndices().size()));

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.getMeshes()[mesh];
            if (range.mVertexCount == 0)
                continue;

            mPositions.writeAt(
                range.mVertexOffset, scene.getPositions().subspan(range.mVertexOffset, range.mVertexCount));
            mIndices.writeAt(range.mIndexOffset, scene.getIndices().subspan(range.mIndexOffset, range.mIndexCount));
        }
    }

    void SceneAcceleration::release(std::span<const Index> meshes)
    {
        for (const Index mesh : meshes)
        {
            // A slot this never held: a scene can add a mesh and sweep it in the same window,
            // before anything was handed over to build it.
            if (mesh >= mBottomLevel.size())
                continue;

            if (mBottomLevel[mesh] != VK_NULL_HANDLE)
                mDevice.getFunctions().mDestroyAccelerationStructure(mDevice.getHandle(), mBottomLevel[mesh], nullptr);

            mBottomLevel[mesh] = VK_NULL_HANDLE;
            mBottomLevelAddresses[mesh] = 0;
            mBottomLevelStorage.give(mBottomLevelRooms[mesh]);
            mBottomLevelRooms[mesh] = StructureRoom{};

            releaseMicromap(mesh);
        }
    }

    void SceneAcceleration::releaseMicromap(Index mesh)
    {
        if (mesh >= mMicromaps.size() || mMicromaps[mesh] == VK_NULL_HANDLE)
            return;

        mDevice.getFunctions().mDestroyMicromap(mDevice.getHandle(), mMicromaps[mesh], nullptr);
        mMicromaps[mesh] = VK_NULL_HANDLE;
        mMicromapStorage.give(mMicromapRooms[mesh]);
        mMicromapRooms[mesh] = StructureRoom{};
        mMicromapUsage[mesh].mCount = 0;
        mMicromapTallies[mesh] = MicromapTally{};
    }

    void SceneAcceleration::attachMicromap(Index mesh, VkAccelerationStructureGeometryKHR& geometry,
        VkAccelerationStructureTrianglesOpacityMicromapEXT& link) const
    {
        if (mesh >= mMicromaps.size() || mMicromaps[mesh] == VK_NULL_HANDLE)
            return;

        link = VkAccelerationStructureTrianglesOpacityMicromapEXT{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT,
            // No index buffer: a triangle's micromap entry is its own position in the geometry,
            // which is the order `Micromap` writes them in.
            .indexType = VK_INDEX_TYPE_NONE_KHR,
            .usageCountsCount = mMicromapUsage[mesh].mCount,
            .pUsageCounts = mMicromapUsage[mesh].mCounts.data(),
            .micromap = mMicromaps[mesh],
        };
        geometry.geometry.triangles.pNext = &link;

        // **The opaque flag the micromap replaces is cleared rather than left standing.** The
        // micromap is read instead of it, so either spelling traces the same; what the clearing buys
        // is the failure mode. A micromap that went missing over a geometry still claiming to be
        // opaque turns a canopy into a solid card, where one over a geometry that does not claim it
        // makes every hit a candidate — which is what the whole cell did before there were any.
        geometry.flags = 0;
    }

    MicromapTally SceneAcceleration::getMicromapTally() const
    {
        MicromapTally total;
        for (const MicromapTally& mesh : mMicromapTallies)
        {
            total.mOpaque += mesh.mOpaque;
            total.mTransparent += mesh.mTransparent;
            total.mUnknown += mesh.mUnknown;
        }

        return total;
    }

    void SceneAcceleration::extend(Batch& batch, const SceneDesc& scene, std::span<const TextureData> textures)
    {
        // **Departures first, so the room they give back is there for the arrivals.** The two lists
        // are disjoint, so a slot handed out again appears only among the arrivals and is dealt with
        // by `buildMeshes`, which destroys whatever the slot was holding.
        release(scene.getFreedMeshes());

        writeGeometry(scene, scene.getArrivedMeshes());
        buildMicromaps(batch, scene, textures, scene.getArrivedMeshes());
        buildMeshes(batch, scene, scene.getArrivedMeshes());
    }

    void SceneAcceleration::buildMicromaps(
        Batch& batch, const SceneDesc& scene, std::span<const TextureData> textures, std::span<const Index> meshes)
    {
        const DeviceFunctions& functions = mDevice.getFunctions();
        const std::size_t slots = scene.getMeshes().size();

        mMicromaps.resize(slots, VK_NULL_HANDLE);
        mMicromapRooms.resize(slots);
        mMicromapUsage.resize(slots);
        mMicromapTallies.resize(slots);

        // A slot handed out again is holding whatever the mesh before it was classified as.
        for (const Index mesh : meshes)
            releaseMicromap(mesh);

        // **No mask, no micromap, and that is a whole answer.** An extend describes the textures
        // that arrived with it, so a mesh appearing beside an image already resident has nothing
        // here to classify against and goes on asking — which is exactly what it did before any of
        // this existed, and what the next reset will settle.
        if (textures.empty())
            return;

        const std::uint32_t deviceLevels
            = mDevice.getPhysicalDevice().getProperties().mOpacityMicromap.maxOpacity4StateSubdivisionLevel;

        materialOfEachMesh(scene, mMaterialOfMesh);

        std::vector<PendingMicromap> pending;
        std::vector<BoundedMask> masks;

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.getMeshes()[mesh];
            if (range.mIndexCount == 0)
                continue;

            const Index material = mMaterialOfMesh[mesh];
            if (material == sNoIndex || material == sManyMaterials)
                continue;

            const Material& worn = scene.getMaterials()[material];
            if (!worn.isCutout())
                continue;

            const TextureData* mask = textureAt(textures, worn.mDiffuse);
            if (mask == nullptr)
                continue;

            const float cutoff = worn.getAlphaCutoff();
            auto held = std::find_if(masks.begin(), masks.end(), [&](const BoundedMask& bounded) {
                return bounded.mTexture == worn.mDiffuse && bounded.mCutoff == cutoff;
            });
            if (held == masks.end())
            {
                masks.push_back(BoundedMask{ worn.mDiffuse, cutoff, AlphaBounds(AlphaImage(*mask), cutoff) });
                held = masks.end() - 1;
            }

            Micromap built(scene.getTexCoords().subspan(range.mVertexOffset, range.mVertexCount),
                scene.getIndices().subspan(range.mIndexOffset, range.mIndexCount), worn.mTextureTransform,
                held->mBounds, deviceLevels);

            // Empty is *build none*: a mask that could not be decoded says nothing about the
            // geometry wearing it, and deciding on its behalf is how a hole gets put through a wall.
            if (!built.isEmpty())
                pending.push_back(PendingMicromap{ mesh, std::move(built) });
        }

        if (pending.empty())
            return;

        // Both inputs are read from addresses that have to be multiples of 256, so each mesh's run
        // starts at one and the buffer carries the pad that puts the first of them there.
        std::vector<VkDeviceSize> triangleOffsets(pending.size());
        std::vector<VkDeviceSize> dataOffsets(pending.size());
        VkDeviceSize triangleBytes = 0;
        VkDeviceSize dataBytes = 0;

        for (std::size_t at = 0; at < pending.size(); ++at)
        {
            const Micromap& micromap = pending[at].mMicromap;

            triangleOffsets[at] = triangleBytes;
            triangleBytes = alignUp(triangleBytes + micromap.getTriangles().size() * sizeof(VkMicromapTriangleEXT),
                sMicromapInputAlignment);

            dataOffsets[at] = dataBytes;
            dataBytes = alignUp(dataBytes + micromap.getData().size(), sMicromapInputAlignment);
        }

        HostBuffer triangleArray(mDevice, triangleBytes + sMicromapInputAlignment, sMicromapInputUsage);
        HostBuffer states(mDevice, dataBytes + sMicromapInputAlignment, sMicromapInputUsage);

        const VkDeviceSize trianglePad = padTo(triangleArray.getDeviceAddress(), sMicromapInputAlignment);
        const VkDeviceSize statePad = padTo(states.getDeviceAddress(), sMicromapInputAlignment);

        std::vector<VkMicromapTriangleEXT> rows;
        for (std::size_t at = 0; at < pending.size(); ++at)
        {
            const Micromap& micromap = pending[at].mMicromap;

            // The core carries no format, because a micromap here has exactly one: four states, of
            // which this only ever writes three.
            rows.clear();
            rows.reserve(micromap.getTriangles().size());
            for (const MicromapTriangle& triangle : micromap.getTriangles())
                rows.push_back(VkMicromapTriangleEXT{
                    .dataOffset = triangle.mDataOffset,
                    .subdivisionLevel = triangle.mSubdivisionLevel,
                    .format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT,
                });

            triangleArray.writeAt(trianglePad + triangleOffsets[at], std::span<const VkMicromapTriangleEXT>(rows));
            states.writeAt(statePad + dataOffsets[at], micromap.getData());
        }

        const VkDeviceSize scratchAlignment
            = mDevice.getPhysicalDevice()
                  .getProperties()
                  .mAccelerationStructure.minAccelerationStructureScratchOffsetAlignment;

        std::vector<VkMicromapBuildInfoEXT> builds(pending.size());
        std::vector<VkDeviceSize> scratchOffsets(pending.size());
        std::vector<VkDeviceSize> sizes(pending.size());
        VkDeviceSize scratchTotal = 0;
        VkDeviceSize wanted = 0;

        for (std::size_t at = 0; at < pending.size(); ++at)
        {
            const Index mesh = pending[at].mMesh;
            const Micromap& micromap = pending[at].mMicromap;

            MicromapUsageCounts& usage = mMicromapUsage[mesh];
            usage.mCount = 0;
            for (const MicromapUsage& used : micromap.getUsage())
            {
                assert(usage.mCount < usage.mCounts.size() && "a level past the ceiling the classifier keeps");
                usage.mCounts[usage.mCount++] = VkMicromapUsageEXT{
                    .count = used.mCount,
                    .subdivisionLevel = used.mSubdivisionLevel,
                    .format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT,
                };
            }

            builds[at] = VkMicromapBuildInfoEXT{
                .sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT,
                .type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT,
                .flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT,
                .mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT,
                .usageCountsCount = usage.mCount,
                .pUsageCounts = usage.mCounts.data(),
                .data = { .deviceAddress = states.getDeviceAddress() + statePad + dataOffsets[at] },
                .triangleArray
                = { .deviceAddress = triangleArray.getDeviceAddress() + trianglePad + triangleOffsets[at] },
                .triangleArrayStride = sizeof(VkMicromapTriangleEXT),
            };

            VkMicromapBuildSizesInfoEXT size{ .sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
            functions.mGetMicromapBuildSizes(
                mDevice.getHandle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &builds[at], &size);

            sizes[at] = size.micromapSize;
            wanted = alignUp(wanted + size.micromapSize, sStructureAlignment);
            scratchOffsets[at] = scratchTotal;
            scratchTotal = alignUp(scratchTotal + size.buildScratchSize, scratchAlignment);
        }

        // A driver may want no scratch at all for micromaps this small; a buffer of nothing is not
        // one that can be made, and the address of this one is then never read.
        Buffer scratch(
            mDevice, std::max<VkDeviceSize>(scratchTotal, 1), sScratchUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkDeviceAddress scratchAddress = scratch.getDeviceAddress();

        std::vector<VkMicromapBuildInfoEXT> live;
        live.reserve(pending.size());

        for (std::size_t at = 0; at < pending.size(); ++at)
        {
            const Index mesh = pending[at].mMesh;

            // A driver answering nought is one that will not build this, and a micromap has to be
            // created at a size before it can be built at all.
            if (sizes[at] == 0)
            {
                mMicromapUsage[mesh].mCount = 0;
                continue;
            }

            mMicromapRooms[mesh] = mMicromapStorage.take(mDevice, sizes[at], wanted);

            const VkMicromapCreateInfoEXT create{
                .sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT,
                .buffer = mMicromapStorage.getBuffer(mMicromapRooms[mesh]),
                .offset = mMicromapStorage.getOffset(mMicromapRooms[mesh]),
                .size = sizes[at],
                .type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT,
            };
            checkVk(functions.mCreateMicromap(mDevice.getHandle(), &create, nullptr, &mMicromaps[mesh]),
                "vkCreateMicromapEXT");

            builds[at].dstMicromap = mMicromaps[mesh];
            builds[at].scratchData.deviceAddress = scratchAddress + scratchOffsets[at];
            mMicromapTallies[mesh] = pending[at].mMicromap.getTally();
            live.push_back(builds[at]);
        }

        if (live.empty())
            return;

        const VkCommandBuffer commands = batch.getCommands();
        functions.mCmdBuildMicromaps(commands, static_cast<std::uint32_t>(live.size()), live.data());
        barrierAfterMicromaps(commands);

        batch.keep(std::move(scratch));
        batch.keep(std::move(triangleArray));
        batch.keep(std::move(states));
    }

    void SceneAcceleration::buildMeshes(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes)
    {
        const DeviceFunctions& functions = mDevice.getFunctions();
        const std::size_t slots = scene.getMeshes().size();

        // Grown to what the scene now holds, never shrunk: a slot the scene took back keeps its
        // index, and the tables below are indexed by it.
        mBottomLevel.resize(slots, VK_NULL_HANDLE);
        mBottomLevelAddresses.resize(slots, 0);
        mBottomLevelRooms.resize(slots);
        mBuildScratch.resize(slots, 0);

        // The build reads these through pointers it keeps until the command is recorded, so they
        // live across the whole function rather than inside the loop.
        mBuildGeometries.assign(meshes.size(), VkAccelerationStructureGeometryKHR{});
        mBuildMicromapLinks.assign(meshes.size(), VkAccelerationStructureTrianglesOpacityMicromapEXT{});
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
        std::vector<VkDeviceSize> scratchOffsets(meshes.size());
        VkDeviceSize scratchTotal = 0;

        for (std::size_t at = 0; at < meshes.size(); ++at)
        {
            const Index slot = meshes[at];
            const MeshRange& mesh = scene.getMeshes()[slot];

            // **A slot handed out again arrives holding different geometry.** Whatever was there is
            // destroyed and its room given back before this one asks for room of its own, so the
            // two can be the same run.
            if (mBottomLevel[slot] != VK_NULL_HANDLE)
            {
                functions.mDestroyAccelerationStructure(mDevice.getHandle(), mBottomLevel[slot], nullptr);
                mBottomLevel[slot] = VK_NULL_HANDLE;
                mBottomLevelAddresses[slot] = 0;
                mBottomLevelStorage.give(mBottomLevelRooms[slot]);
                mBottomLevelRooms[slot] = StructureRoom{};
            }

            // Indices are mesh-local, so each structure is handed the slice of the shared buffers
            // that belongs to it and addresses vertex zero as its own first vertex.
            mBuildGeometries[at] = VkAccelerationStructureGeometryKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .geometry = { .triangles = {
                                  .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                  .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                  .vertexData = { .deviceAddress = mesh.mVertexCount > 0
                                          ? mPositions.addressOf(mesh.mVertexOffset)
                                          : 0 },
                                  .vertexStride = sizeof(osg::Vec3f),
                                  // **Guarded, because a freed slot has no vertices.** A slot the
                                  // scene has taken back keeps its index and its room and holds a
                                  // count of zero until something fits into it; subtracting one
                                  // there wraps, and the driver is handed four billion vertices.
                                  .maxVertex = mesh.mVertexCount > 0 ? mesh.mVertexCount - 1 : 0,
                                  .indexType = VK_INDEX_TYPE_UINT32,
                                  .indexData = { .deviceAddress
                                      = mesh.mIndexCount > 0 ? mIndices.addressOf(mesh.mIndexOffset) : 0 },
                              } },
                // Opaque as built, and overridden per instance where a material says otherwise:
                // opacity is a property of the material and a mesh does not carry one, so the
                // top-level flags are the only place the question can be answered exactly — unless
                // a micromap answers it below, which is the one case where the mesh does carry it.
                .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
            };

            attachMicromap(slot, mBuildGeometries[at], mBuildMicromapLinks[at]);

            mBuilds[at] = VkAccelerationStructureBuildGeometryInfoKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                // ALLOW_DATA_ACCESS is what lets a shader read a hit triangle's vertices back out of
                // the structure, which is the whole reason nothing here binds a vertex buffer.
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                    | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR,
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
                mBuildScratch[slot] = 0;
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
            wanted = alignUp(wanted + sizes.accelerationStructureSize, sStructureAlignment);

            scratchOffsets[at] = scratchTotal;
            scratchTotal = alignUp(scratchTotal + sizes.buildScratchSize, scratchAlignment);

            // Kept so a rebuild of this one mesh does not have to ask the driver its size again.
            // The same geometry describes it, so the answer cannot have changed.
            mBuildScratch[slot] = sizes.buildScratchSize;

            mBuildRanges[at] = VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = triangles };
        }

        if (scratchTotal == 0)
            return;

        // Scratch is transient: it is read and written by the build and never again. It is handed to
        // the batch below rather than left to this scope, because the build it feeds has only been
        // recorded when this function returns — and the batch frees it the moment the flush does.
        Buffer scratch(mDevice, scratchTotal, sScratchUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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
            mBuilds[at].scratchData.deviceAddress = scratchAddress + scratchOffsets[at];

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

    void SceneAcceleration::prepareRefit(const SceneDesc& scene)
    {
        const std::span<const Index> deformed = scene.getDeformed();
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
            scratchTotal = alignUp(scratchTotal + mBuildScratch[mesh], scratchAlignment);
        }

        if (mRefitScratch.getSize() < scratchTotal)
            mRefitScratch = Buffer(mDevice, scratchTotal, sScratchUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        const VkDeviceAddress scratchAddress = mRefitScratch.getDeviceAddress();

        mRefitGeometries.resize(count);
        mRefitMicromapLinks.assign(count, VkAccelerationStructureTrianglesOpacityMicromapEXT{});
        mRefitBuilds.resize(count);
        mRefitRanges.resize(count);
        mRefitRangePointers.resize(count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            const Index index = deformed[i];
            const MeshRange& mesh = scene.getMeshes()[index];

            // **Straight into the memory the builder reads**, with no staging buffer between and no
            // copy to record. The submit below carries an implicit dependency on host writes made
            // before it, which is what a barrier would otherwise have been for.
            mPositions.writeAt(mesh.mVertexOffset, scene.getMeshPositions(index));

            // The same description the first build was given, which is what makes the structure it
            // produces the same size as the one already sitting at this mesh's offset.
            mRefitGeometries[i] = VkAccelerationStructureGeometryKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .geometry = { .triangles = {
                                  .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                  .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                  .vertexData = { .deviceAddress = mPositions.addressOf(mesh.mVertexOffset) },
                                  .vertexStride = sizeof(osg::Vec3f),
                                  // **Guarded, because a freed slot has no vertices.** A slot the
                                  // scene has taken back keeps its index and its room and holds a
                                  // count of zero until something fits into it; subtracting one
                                  // there wraps, and the driver is handed four billion vertices.
                                  .maxVertex = mesh.mVertexCount > 0 ? mesh.mVertexCount - 1 : 0,
                                  .indexType = VK_INDEX_TYPE_UINT32,
                                  .indexData = { .deviceAddress = mIndices.addressOf(mesh.mIndexOffset) },
                              } },
                .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
            };

            attachMicromap(index, mRefitGeometries[i], mRefitMicromapLinks[i]);

            mRefitRanges[i] = VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = mesh.getTriangleCount() };
            mRefitRangePointers[i] = &mRefitRanges[i];
        }

        // A second pass, because `pGeometries` is a pointer into a vector the first pass was still
        // filling: a build info written beside a geometry that later moved would name freed memory.
        VkDeviceSize scratchAt = 0;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const Index index = deformed[i];

            // **Built into the structure that is already there**, rather than into a new one beside
            // it. A build overwrites its destination outright, the geometry it is given is the same
            // shape as last time so it needs no more room, and the structure's handle is what every
            // top-level instance already points at.
            mRefitBuilds[i] = VkAccelerationStructureBuildGeometryInfoKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                    | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .dstAccelerationStructure = mBottomLevel[index],
                .geometryCount = 1,
                .pGeometries = &mRefitGeometries[i],
                .scratchData = { .deviceAddress = scratchAddress + scratchAt },
            };

            scratchAt = alignUp(scratchAt + mBuildScratch[index], scratchAlignment);
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

    void SceneAcceleration::place(
        CommandPool& pool, const SceneDesc& scene, std::span<const InstanceRecord> records, GpuTimer* timer)
    {
        prepareRefit(scene);
        prepareTopLevel(scene, records);

        // **One submit, and the barrier between them is what the fence used to be.** The top level
        // is built over structures the refit has just rewritten, which is a dependency inside a
        // command buffer rather than a reason to go round the driver twice.
        pool.submitAndWait([&](VkCommandBuffer commands) {
            if (!mRefitBuilds.empty())
                recordRefit(commands, timer);

            recordTopLevel(commands, timer);
        });
    }

    void SceneAcceleration::prepareTopLevel(const SceneDesc& scene, std::span<const InstanceRecord> records)
    {
        const DeviceFunctions& functions = mDevice.getFunctions();

        // **Checked here rather than left to the driver.** A scene that grew a mesh since `setScene`
        // built the structures is a caller breaking `placeScene`'s contract, and the only symptom is
        // a top level naming a bottom level that was never made — which surfaces as an invalid handle
        // inside `vkGetAccelerationStructureDeviceAddressKHR` and says nothing about who did it. One
        // comparison, once a frame, for a failure that otherwise takes the process down unexplained.
        if (scene.getMeshes().size() != mBottomLevel.size())
            throw Error("the scene grew from " + std::to_string(mBottomLevel.size()) + " meshes to "
                + std::to_string(scene.getMeshes().size())
                + " without being built again; placeScene can only move what setScene made");

        mCutoutInstanceCount = 0;
        mMicromappedInstanceCount = 0;
        mRowScratch.clear();
        mRowScratch.reserve(records.size());

        for (std::uint32_t index = 0; index < records.size(); ++index)
        {
            const InstanceRecord& record = records[index];

            // **A gap contributes no row rather than a masked one.** Its slot still names it — the
            // custom index below is the slot and not the row — so skipping costs the build one
            // primitive it would otherwise have to sort and never hit.
            if (!record.mPlaced)
                continue;

            // Morrowind's sheet geometry is lit and hit from both faces, so nothing is culled.
            VkGeometryInstanceFlagsKHR flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            if (record.mCutout)
            {
                // Counted here rather than in a pass of its own: this loop already visits every
                // record and skips the same gaps, and a scene is tens of thousands of them.
                ++mCutoutInstanceCount;

                // **A mesh a micromap answers for must not be forced non-opaque.** Forcing overrides
                // what the micromap decided, so every microtriangle it resolved as opaque would go
                // back to asking — the whole of the saving, handed back at the instance. Where there
                // is no micromap the force is what makes the cutout work at all.
                if (record.mMesh < mMicromaps.size() && mMicromaps[record.mMesh] != VK_NULL_HANDLE)
                    ++mMicromappedInstanceCount;
                else
                    flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
            }

            mRowScratch.push_back(VkAccelerationStructureInstanceKHR{
                .transform = toVulkanTransform(record.mTransform),
                // A record's position is the custom index the shader reads back at a hit.
                .instanceCustomIndex = index & 0xFFFFFFu,
                .mask = record.mMask,
                .flags = flags,
                .accelerationStructureReference = mBottomLevelAddresses[record.mMesh],
            });
        }

        mInstanceCount = static_cast<std::uint32_t>(mRowScratch.size());

        // **Written where the builder reads it.** These are rewritten whole every frame, so staging
        // them was a buffer made, a copy recorded, a submit and a wait on the queue for a memcpy.
        const std::span<const VkAccelerationStructureInstanceKHR> rows(mRowScratch);
        growTo(mInstances, mDevice, rows.size_bytes(), sBuildInputUsage);

        mInstances.write(rows);

        mTopLevelGeometry = VkAccelerationStructureGeometryKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry = { .instances = {
                              .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                              .data = { .deviceAddress = mInstances.getDeviceAddress() },
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
        functions.mGetAccelerationStructureBuildSizes(mDevice.getHandle(),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mTopLevelBuild, &mInstanceCount, &sizes);

        // **The old structure goes before the new one is made over the same buffer.** Nothing is
        // reading it: every submit on this path waits, and so does the trace, so by the time a frame
        // is placing the world again the last one has finished with it.
        if (mTopLevel != VK_NULL_HANDLE)
        {
            functions.mDestroyAccelerationStructure(mDevice.getHandle(), mTopLevel, nullptr);
            mTopLevel = VK_NULL_HANDLE;
        }

        mTopLevelBytes = sizes.accelerationStructureSize;

        // Grown to the high-water mark and kept, both of them. A structure is created at offset zero
        // of whatever this holds and asks only that it be large enough.
        if (mTopLevelStorage.getSize() < sizes.accelerationStructureSize)
            mTopLevelStorage
                = Buffer(mDevice, sizes.accelerationStructureSize, sStorageUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (mTopLevelScratch.getSize() < sizes.buildScratchSize)
            mTopLevelScratch
                = Buffer(mDevice, sizes.buildScratchSize, sScratchUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        const VkAccelerationStructureCreateInfoKHR create{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = mTopLevelStorage.getHandle(),
            .size = sizes.accelerationStructureSize,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        };
        checkVk(functions.mCreateAccelerationStructure(mDevice.getHandle(), &create, nullptr, &mTopLevel),
            "vkCreateAccelerationStructureKHR");
        mDevice.setName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<std::uint64_t>(mTopLevel), "scene");
        mDevice.setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(mInstances.getHandle()), "instances");

        mTopLevelBuild.dstAccelerationStructure = mTopLevel;
        mTopLevelBuild.scratchData.deviceAddress = mTopLevelScratch.getDeviceAddress();
    }

    void SceneAcceleration::recordTopLevel(VkCommandBuffer commands, GpuTimer* timer)
    {
        const VkAccelerationStructureBuildRangeInfoKHR range{ .primitiveCount = mInstanceCount };
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

        openZone(timer, commands, "tlas");
        mDevice.getFunctions().mCmdBuildAccelerationStructures(commands, 1, &mTopLevelBuild, &ranges);
        barrierAfterBuild(commands);
        closeZone(timer, commands);
    }
}
