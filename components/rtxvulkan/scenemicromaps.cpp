#include "scenemicromaps.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include <components/rtx/error.hpp>

#include "commands.hpp"
#include "device.hpp"
#include "gputimer.hpp"
#include "graveyard.hpp"
#include "micromappass.hpp"
#include "result.hpp"
#include "sceneacceleration.hpp"
#include "scenebuffers.hpp"
#include "texture.hpp"

namespace Rtx
{
    namespace
    {
        /// What a build's two inputs are created with, and what each one's address must be a
        /// multiple of.
        constexpr VkBufferUsageFlags sInputUsage
            = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        constexpr VkDeviceSize sInputAlignment = 256;

        constexpr VkBufferUsageFlags sScratchUsage
            = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        /// The level a triangle covering `texels` texels of its mask is cut at: one microtriangle
        /// about two texels across, which is the SDK's and Indiana Jones' number, between the floor
        /// `micromap.h` gives a reason for and the cap the memory arithmetic makes necessary.
        ///
        /// `4^N` microtriangles over `texels` texels is four apiece at `N = log4(texels / 4)`.
        std::uint32_t levelFor(float texels)
        {
            if (!(texels > 4.0f))
                return Shaders::MICROMAP_LEVEL_MIN;

            // A triangle the cap cannot cut down to the budget is unknown at every microtriangle
            // whatever its level, so it takes the floor: one word rather than two hundred and
            // fifty-six, and sixteen sparse means rather than four thousand.
            const auto finest = static_cast<float>(Shaders::microtriangleCount(Shaders::MICROMAP_LEVEL_MAX));
            if (texels > static_cast<float>(Shaders::MICROMAP_TEXEL_BUDGET) * finest)
                return Shaders::MICROMAP_LEVEL_MIN;

            const float wanted = 0.5f * std::log2(texels / 4.0f);
            const auto rounded = static_cast<std::uint32_t>(std::lround(wanted));

            return std::clamp(rounded, Shaders::MICROMAP_LEVEL_MIN, Shaders::MICROMAP_LEVEL_MAX);
        }

        /// A mesh's texture coordinate on the material's own sheet, as `texturePoint` lands a hit.
        osg::Vec2f onSheet(const osg::Vec2f& uv, const osg::Vec4f& transform)
        {
            return osg::Vec2f(uv.x() * transform.x() + transform.z(), uv.y() * transform.y() + transform.w());
        }

        /// Orders the kernel's writes against the build that reads them as its data.
        void barrierBeforeMicromapBuild(VkCommandBuffer commands)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
            };
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(commands, &dependency);
        }

        /// Orders the build against the structure builds that attach what it wrote, and against
        /// the traces that read it through them.
        void barrierAfterMicromapBuild(VkCommandBuffer commands)
        {
            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
                .srcAccessMask = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                    | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
            };
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(commands, &dependency);
        }
    }

    SceneMicromaps::SceneMicromaps(const Device& device)
        : mDevice(device)
    {
        const std::uint32_t finest
            = device.getPhysicalDevice().getProperties().mOpacityMicromap.maxOpacity4StateSubdivisionLevel;
        if (finest < Shaders::MICROMAP_LEVEL_MAX)
            throw Unsupported("the device cuts a four-state micromap triangle to level " + std::to_string(finest)
                + " at most, and the bake wants " + std::to_string(Shaders::MICROMAP_LEVEL_MAX));
    }

    SceneMicromaps::~SceneMicromaps()
    {
        const DeviceFunctions& functions = mDevice.getFunctions();
        for (const MeshMicromap& held : mMeshes)
            if (held.mHandle != VK_NULL_HANDLE)
                functions.mDestroyMicromap(mDevice.getHandle(), held.mHandle, nullptr);
    }

    SceneMicromaps::Baked SceneMicromaps::bakedOf(const Material& material, const Index index)
    {
        return Baked{
            .mMaterial = index,
            .mDiffuse = material.mDiffuse,
            .mCutoff = material.getAlphaCutoff(),
            .mTransform = material.mTextureTransform,
            .mTranslucent = material.isTranslucent(),
        };
    }

    void SceneMicromaps::drop(const Index mesh, Graveyard& graveyard)
    {
        if (mesh >= mMeshes.size())
            return;

        MeshMicromap& held = mMeshes[mesh];
        if (held.mHandle == VK_NULL_HANDLE)
            return;

        graveyard.bury(held.mHandle);
        graveyard.bury(mStorage, held.mRoom);
        held = MeshMicromap{};
    }

    void SceneMicromaps::release(std::span<const Index> meshes, Graveyard& graveyard)
    {
        for (const Index mesh : meshes)
            drop(mesh, graveyard);
    }

    VkAccelerationStructureTrianglesOpacityMicromapEXT SceneMicromaps::describe(const Index mesh) const
    {
        assert(has(mesh) && "a micromap described for a mesh that carries none");
        const MeshMicromap& held = mMeshes[mesh];

        return VkAccelerationStructureTrianglesOpacityMicromapEXT{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT,
            .indexType = VK_INDEX_TYPE_NONE_KHR,
            .baseTriangle = 0,
            .usageCountsCount = held.mUsageCount,
            .pUsageCounts = held.mUsage.data(),
            .micromap = held.mHandle,
        };
    }

    void SceneMicromaps::check(const SceneDesc& scene)
    {
        const std::span<const Index> written = scene.getWrittenMaterials();
        const std::span<const Material> materials = scene.getMaterials();

        // The ordinary frame: a few flipbooks and scrolls, none of which anything bakes against.
        bool suspect = false;
        for (const Index at : written)
            suspect = suspect || !materials[at].mAnimated;
        if (!suspect)
            return;

        mWrittenScratch.assign(materials.size(), 0);
        for (const Index at : written)
            mWrittenScratch[at] = 1;

        for (std::size_t mesh = 0; mesh < mMeshes.size(); ++mesh)
        {
            const MeshMicromap& held = mMeshes[mesh];
            if (held.mHandle == VK_NULL_HANDLE || mWrittenScratch[held.mBaked.mMaterial] == 0)
                continue;

            const Material& now = materials[held.mBaked.mMaterial];
            const Baked rewritten = bakedOf(now, held.mBaked.mMaterial);
            if (rewritten != held.mBaked)
                throw Error("material " + std::to_string(held.mBaked.mMaterial)
                    + " was rewritten under the micromap baked against it for mesh " + std::to_string(mesh)
                    + ": diffuse " + std::to_string(held.mBaked.mDiffuse) + " -> " + std::to_string(rewritten.mDiffuse)
                    + ", cutoff " + std::to_string(held.mBaked.mCutoff) + " -> " + std::to_string(rewritten.mCutoff)
                    + ", translucent " + std::to_string(held.mBaked.mTranslucent) + " -> "
                    + std::to_string(rewritten.mTranslucent) + ", transform "
                    + std::to_string(held.mBaked.mTransform.x()) + "," + std::to_string(held.mBaked.mTransform.y())
                    + "," + std::to_string(held.mBaked.mTransform.z()) + ","
                    + std::to_string(held.mBaked.mTransform.w()) + " -> " + std::to_string(rewritten.mTransform.x())
                    + "," + std::to_string(rewritten.mTransform.y()) + "," + std::to_string(rewritten.mTransform.z())
                    + "," + std::to_string(rewritten.mTransform.w()) + ", animated " + std::to_string(now.mAnimated)
                    + ", kind " + std::to_string(static_cast<int>(now.mKind)) + ", the mesh wears "
                    + std::to_string(scene.getMeshes()[mesh].mMaterial) + " with "
                    + std::to_string(scene.getMeshes()[mesh].mVertexCount) + " vertices");
        }
    }

    void SceneMicromaps::bake(Batch& batch, const MicromapPass& pass, const SceneDesc& scene,
        const SceneBuffers& buffers, const SceneAcceleration& acceleration, const TextureArray& textures,
        std::span<const Index> meshes, GpuTimer* const timer, Graveyard& graveyard)
    {
        const std::span<const MeshRange> ranges = scene.getMeshes();
        const std::span<const Material> materials = scene.getMaterials();
        const std::span<const osg::Vec2f> texCoords = scene.getTexCoords();
        const std::span<const std::uint32_t> indices = scene.getIndices();

        // Grown to what the scene now holds, never shrunk: a slot the scene took back keeps its
        // index. **Here and nowhere else**, for the reason `describe` gives.
        if (mMeshes.size() < ranges.size())
            mMeshes.resize(ranges.size());

        mPlanned.clear();
        mTriangleScratch.clear();

        VkDeviceSize dataTotal = 0;
        VkDeviceSize triangleTotal = 0;

        for (const Index slot : meshes)
        {
            drop(slot, graveyard);

            const MeshRange& mesh = ranges[slot];
            if (mesh.mVertexCount == 0 || mesh.mMaterial == sNoIndex)
                continue;

            const Material& material = materials[mesh.mMaterial];
            if (!material.isCutout() || material.isTranslucent() || material.mAnimated)
                continue;

            const VkExtent2D extent = textures.getExtent(material.mDiffuse);
            if (extent.width == 0 || extent.height == 0)
            {
                ++mUntextured;
                continue;
            }

            const float texelsOnSheet = static_cast<float>(extent.width) * static_cast<float>(extent.height);
            const std::uint32_t triangles = mesh.getTriangleCount();

            Planned plan{
                .mMesh = slot,
                .mTriangleAt = static_cast<std::uint32_t>(mTriangleScratch.size()),
                .mData = dataTotal,
                .mTriangles = triangleTotal,
            };

            // **The level is chosen here per triangle, because the build's usage counts must be
            // exact and the host writes them.** A triangle's data is `4^N / 4` bytes, laid end to
            // end from a word boundary, which `micromap.h`'s floor keeps every run on.
            std::array<std::uint32_t, sLevelCount> counts{};
            std::uint32_t bytes = 0;
            for (std::uint32_t triangle = 0; triangle < triangles; ++triangle)
            {
                const std::uint32_t* corner = &indices[mesh.mIndexOffset + std::size_t{ triangle } * 3];
                const osg::Vec2f a = onSheet(texCoords[mesh.mVertexOffset + corner[0]], material.mTextureTransform);
                const osg::Vec2f b = onSheet(texCoords[mesh.mVertexOffset + corner[1]], material.mTextureTransform);
                const osg::Vec2f c = onSheet(texCoords[mesh.mVertexOffset + corner[2]], material.mTextureTransform);

                const float doubled = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y()));
                const std::uint32_t level = levelFor(0.5f * doubled * texelsOnSheet);

                mTriangleScratch.push_back(VkMicromapTriangleEXT{
                    .dataOffset = bytes,
                    .subdivisionLevel = static_cast<std::uint16_t>(level),
                    .format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT,
                });
                bytes += Shaders::microtriangleWords(level) * static_cast<std::uint32_t>(sizeof(std::uint32_t));
                ++counts[level - Shaders::MICROMAP_LEVEL_MIN];
            }

            MeshMicromap& held = mMeshes[slot];
            held.mUsageCount = 0;
            for (std::uint32_t at = 0; at < sLevelCount; ++at)
                if (counts[at] > 0)
                    held.mUsage[held.mUsageCount++] = VkMicromapUsageEXT{
                        .count = counts[at],
                        .subdivisionLevel = at + Shaders::MICROMAP_LEVEL_MIN,
                        .format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT,
                    };
            held.mBaked = bakedOf(material, mesh.mMaterial);

            dataTotal = alignUp(dataTotal + bytes, sInputAlignment);
            triangleTotal
                = alignUp(triangleTotal + VkDeviceSize{ triangles } * sizeof(VkMicromapTriangleEXT), sInputAlignment);
            mPlanned.push_back(plan);
        }

        if (mPlanned.empty())
            return;

        const DeviceFunctions& functions = mDevice.getFunctions();
        const VkDeviceSize scratchAlignment
            = mDevice.getPhysicalDevice()
                  .getProperties()
                  .mAccelerationStructure.minAccelerationStructureScratchOffsetAlignment;

        // The build's inputs, transient like the structures' scratch: the data written by the
        // dispatches below, the triangle array written by the host, both handed to the batch so they
        // outlive the builds that read them and go the moment the flush does.
        Buffer data = Buffer::deviceLocal(mDevice, dataTotal, sInputUsage);
        Buffer triangleArray = Buffer::hostWritten(mDevice, triangleTotal, sInputUsage);

        // Each `Buffer` is its own allocation bound at offset zero, which every driver aligns far
        // more coarsely than this; asserted rather than worked around, as the shader table's is.
        assert(data.getDeviceAddress() % sInputAlignment == 0 && triangleArray.getDeviceAddress() % sInputAlignment == 0
            && "a build input the device would not read from where it was put");

        mBuildScratch.assign(mPlanned.size(), VkMicromapBuildInfoEXT{});

        VkDeviceSize scratchTotal = 0;
        VkDeviceSize wanted = 0;
        for (std::size_t at = 0; at < mPlanned.size(); ++at)
        {
            Planned& plan = mPlanned[at];
            const MeshMicromap& held = mMeshes[plan.mMesh];
            const std::uint32_t triangles = ranges[plan.mMesh].getTriangleCount();

            triangleArray.writeAt(plan.mTriangles,
                std::span<const VkMicromapTriangleEXT>(mTriangleScratch).subspan(plan.mTriangleAt, triangles));

            VkMicromapBuildInfoEXT& info = mBuildScratch[at];
            info = VkMicromapBuildInfoEXT{
                .sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT,
                .type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT,
                .flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT,
                .mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT,
                .usageCountsCount = held.mUsageCount,
                .pUsageCounts = held.mUsage.data(),
                .data = { .deviceAddress = data.getDeviceAddress() + plan.mData },
                .triangleArray = { .deviceAddress = triangleArray.getDeviceAddress() + plan.mTriangles },
                .triangleArrayStride = sizeof(VkMicromapTriangleEXT),
            };
            plan.mSizes = VkMicromapBuildSizesInfoEXT{ .sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
            functions.mGetMicromapBuildSizes(
                mDevice.getHandle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &plan.mSizes);

            wanted = alignUp(wanted + plan.mSizes.micromapSize, StructureStorage::sAlignment);
            plan.mScratch = scratchTotal;
            scratchTotal = alignUp(scratchTotal + plan.mSizes.buildScratchSize, scratchAlignment);
        }

        // A byte where no build wants any: a buffer of nothing is not one Vulkan will make, and the
        // address still has to be one.
        Buffer scratch = Buffer::deviceLocal(mDevice, std::max<VkDeviceSize>(scratchTotal, 1), sScratchUsage);

        for (std::size_t at = 0; at < mPlanned.size(); ++at)
        {
            const Planned& plan = mPlanned[at];
            MeshMicromap& held = mMeshes[plan.mMesh];
            held.mRoom = mStorage.take(mDevice, plan.mSizes.micromapSize, wanted);

            const VkMicromapCreateInfoEXT create{
                .sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT,
                .buffer = mStorage.getBuffer(held.mRoom),
                .offset = mStorage.getOffset(held.mRoom),
                .size = plan.mSizes.micromapSize,
                .type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT,
            };
            checkVk(
                functions.mCreateMicromap(mDevice.getHandle(), &create, nullptr, &held.mHandle), "vkCreateMicromapEXT");
            mDevice.setName(VK_OBJECT_TYPE_MICROMAP_EXT, reinterpret_cast<std::uint64_t>(held.mHandle),
                "micromap " + std::to_string(plan.mMesh));

            mBuildScratch[at].dstMicromap = held.mHandle;
            mBuildScratch[at].scratchData.deviceAddress = scratch.getDeviceAddress() + plan.mScratch;
        }

        // Every slot this bake reads, once each, for the set of its own it reads them through.
        mSlotScratch.clear();
        for (const Planned& plan : mPlanned)
            mSlotScratch.push_back(materials[ranges[plan.mMesh].mMaterial].mDiffuse);
        std::sort(mSlotScratch.begin(), mSlotScratch.end());
        mSlotScratch.erase(std::unique(mSlotScratch.begin(), mSlotScratch.end()), mSlotScratch.end());

        // **A set of its own and not the array's, because the array is written while the bake is
        // pending.** An arrival's batch rides the next submit rather than waiting, and a composite
        // landing in the same frame — the walk hands the scene over twice at a crossing — writes
        // the array's set before that submit has run. `TextureArray::describeApart` says what a
        // pending dispatch may read through; the pool goes to the graveyard with the batch, and
        // the frame's fence frees it.
        const SetApart masks = textures.describeApart(mSlotScratch);
        graveyard.bury(masks.mPool);

        const VkCommandBuffer commands = batch.getCommands();
        openZone(timer, commands, "micromap");

        pass.begin(commands, masks.mSet);
        for (const Planned& plan : mPlanned)
        {
            const MeshRange& mesh = ranges[plan.mMesh];
            const Material& material = materials[mesh.mMaterial];

            pass.bake(commands,
                Shaders::MicromapConstants{
                    .mIndices = acceleration.getIndices(mesh),
                    .mTexCoords = buffers.getTexCoords(mesh),
                    .mTriangles = triangleArray.getDeviceAddress() + plan.mTriangles,
                    .mData = data.getDeviceAddress() + plan.mData,
                    .mTransform = material.mTextureTransform,
                    .mTexture = material.mDiffuse,
                    .mCount = mesh.getTriangleCount(),
                    .mCutoff = material.getAlphaCutoff(),
                    .mPadding = 0,
                });
        }

        barrierBeforeMicromapBuild(commands);
        functions.mCmdBuildMicromaps(commands, static_cast<std::uint32_t>(mBuildScratch.size()), mBuildScratch.data());
        barrierAfterMicromapBuild(commands);
        closeZone(timer, commands);

        batch.keep(std::move(data));
        batch.keep(std::move(triangleArray));
        batch.keep(std::move(scratch));
    }
}
