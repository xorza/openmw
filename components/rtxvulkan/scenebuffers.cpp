#include "scenebuffers.hpp"

#include <algorithm>
#include <string>

#include <components/rtx/instancerecord.hpp>

#include <array>
#include <vector>

#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/wavespectrum.hpp>

#include "commands.hpp"
#include "device.hpp"

namespace Rtx
{
    namespace
    {
        constexpr VkBufferUsageFlags sTableUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        // A shader cannot see a C++ enum, so the two spellings of the same three values are pinned
        // here rather than trusted to stay in step.
        static_assert(static_cast<std::uint32_t>(MaterialKind::Surface) == Shaders::KIND_SURFACE);
        static_assert(static_cast<std::uint32_t>(MaterialKind::Terrain) == Shaders::KIND_TERRAIN);
        static_assert(static_cast<std::uint32_t>(MaterialKind::Water) == Shaders::KIND_WATER);

        Shaders::GpuMaterial toGpu(const Material& material)
        {
            // Zero where the material has no texture to read a mask out of, so that the shader's
            // comparison agrees with `Material::isCutout`, which is what decided whether traversal
            // would ever make it.
            return Shaders::GpuMaterial{
                .mKind = static_cast<std::uint32_t>(material.mKind),
                .mDiffuse = material.mDiffuse,
                .mAlphaCutoff = material.isCutout() ? material.getAlphaCutoff() : 0.0f,
                .mLayerOffset = material.mLayerOffset,
                .mLayerCount = material.mLayerCount,
                .mEmissive = material.mEmissive,
                .mDiffuseColour = material.mDiffuseColour,
                .mEmissiveColour = material.mEmissiveColour,
                .mTextureTransform = material.mTextureTransform,
            };
        }

        Shaders::GpuLight toGpu(const Light& light)
        {
            return Shaders::GpuLight{
                .mPosition = light.mPosition,
                .mIntensity = light.mIntensity,
                .mReach = light.mReach,
                .mRadius = light.mRadius,
            };
        }

        Shaders::GpuSprite toGpu(const Sprite& sprite)
        {
            return Shaders::GpuSprite{
                .mPosition = sprite.mPosition,
                .mRadius = sprite.mRadius,
                .mColour = sprite.mColour,
                .mAlpha = sprite.mAlpha,
                .mMoved = sprite.mMoved,
            };
        }

        Shaders::GpuEmitter toGpu(const SpriteEmitter& emitter)
        {
            return Shaders::GpuEmitter{
                .mCentre = emitter.mCentre,
                .mReach = emitter.mReach,
                .mFirst = emitter.mFirst,
                .mCount = emitter.mCount,
                .mTexture = emitter.mTexture,
                .mAdditive = emitter.mAdditive ? 1u : 0u,
                .mAcross = emitter.mAcross,
                .mUpward = emitter.mUpward,
            };
        }

        Shaders::GpuLayer toGpu(const MaterialLayer& layer)
        {
            return Shaders::GpuLayer{
                .mDiffuse = layer.mDiffuse,
                .mMaskOffset = layer.mMaskOffset,
                .mMaskWidth = layer.mMaskWidth,
                .mMaskHeight = layer.mMaskHeight,
                .mDiffuseTransform = layer.mDiffuseTransform,
                .mMaskTransform = layer.mMaskTransform,
            };
        }
    }

    SceneBuffers::SceneBuffers(const Device& device, Batch& batch, const SceneDesc& scene,
        std::span<const InstanceRecord> records, const SeaState& sea)
        : mDevice(&device)
    {
        mNormals.open(device, sTableUsage, "normals");
        mTexCoords.open(device, sTableUsage, "uvs");

        // **Every table exists from here, whether or not anything has been written to it.** A pass
        // binds all of them and the shader declares all of them; what fills one is a later call that
        // a frame may never make — a scene with no sprites never bins any, and the tiles were then
        // bound as nothing at all. Growing on write cannot carry that guarantee, because the write is
        // exactly what does not happen.
        for (HostBuffer* table : { &mMaterials, &mLayers, &mMasks, &mMeshes, &mInstances, &mLights, &mLightOffsets,
                 &mGrid, &mLightIndices, &mWaves, &mSprites, &mEmitters, &mSpriteTileOffsets, &mSpriteTileIndices })
            growTo(*table, device, 0, sTableUsage);

        // Every mesh the scene holds, which is the same path an arrival takes with a shorter list.
        std::vector<Index> every(scene.getMeshes().size());
        for (std::size_t at = 0; at < every.size(); ++at)
            every[at] = static_cast<Index>(at);

        writeMeshes(scene, every);

        // The shading tables come from `place`, which is also where they are rewritten when a
        // material changes. Forced here because nothing has been written at revision zero.
        mShaded = scene.getShadingRevision() - 1;
        place(scene, records, sea);

        device.setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(mMaterials.getHandle()), "materials");
        device.setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(mInstances.getHandle()), "instance rows");
    }

    void SceneBuffers::extend(const SceneDesc& scene)
    {
        writeMeshes(scene, scene.getArrivedMeshes());
    }

    void SceneBuffers::writeMeshes(const SceneDesc& scene, std::span<const Index> meshes)
    {
        // **Whole runs here and a mesh at a time afterwards.** Only a skinned body's normals change,
        // so filling these when the mesh arrives is a load's cost and every frame after it pays for
        // what actually moved.
        mNormals.reserve(static_cast<std::uint32_t>(scene.getNormals().size()));
        mTexCoords.reserve(static_cast<std::uint32_t>(scene.getTexCoords().size()));

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.getMeshes()[mesh];
            if (range.mVertexCount == 0)
                continue;

            mNormals.writeAt(range.mVertexOffset, scene.getNormals().subspan(range.mVertexOffset, range.mVertexCount));
            mTexCoords.writeAt(
                range.mVertexOffset, scene.getTexCoords().subspan(range.mVertexOffset, range.mVertexCount));
        }

        // **Whole, and it is eight bytes a slot.** A mesh arriving moves nothing already in this,
        // but sizing it to the scene means growing it, and growing means writing it — so the rows
        // that did not change are written again for the price of not having to know which did.
        mMeshScratch.clear();
        mMeshScratch.reserve(scene.getMeshes().size());
        for (const MeshRange& mesh : scene.getMeshes())
            mMeshScratch.push_back(
                Shaders::GpuMesh{ .mVertexOffset = mesh.mVertexOffset, .mIndexOffset = mesh.mIndexOffset });

        reserve(mMeshes, mMeshScratch.size() * sizeof(Shaders::GpuMesh));
        mMeshes.write(std::span<const Shaders::GpuMesh>(mMeshScratch));
        mDevice->setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(mMeshes.getHandle()), "meshes");
    }

    void SceneBuffers::reserve(HostBuffer& held, const VkDeviceSize bytes)
    {
        growTo(held, *mDevice, bytes, sTableUsage);
    }

    void SceneBuffers::binSprites(const osg::Vec3f& origin, const Shaders::Camera& camera)
    {
        mSpriteTiles.rebuild(mSpriteScratch, mEmitterScratch, origin, camera);

        // A frame with no sprites has an empty list, and the offsets are all nought — so the shader
        // reads none of this and `growTo` is what makes sure there is something for it not to read.
        reserve(mSpriteTileOffsets, mSpriteTiles.getOffsets().size_bytes());
        reserve(mSpriteTileIndices, mSpriteTiles.getIndices().size_bytes());

        mSpriteTileOffsets.write(mSpriteTiles.getOffsets());
        mSpriteTileIndices.write(mSpriteTiles.getIndices());
    }

    void SceneBuffers::shade(const SceneDesc& scene)
    {
        if (mShaded == scene.getShadingRevision())
            return;

        mShaded = scene.getShadingRevision();

        mMaterialScratch.clear();
        mMaterialScratch.reserve(scene.getMaterials().size() + 1);
        for (const Material& material : scene.getMaterials())
            mMaterialScratch.push_back(toGpu(material));

        // A drawable with no state set has no material, and `sNoIndex` is not somewhere the shader
        // can be allowed to look. One untextured entry at the end costs less than a branch per hit,
        // and every instance that had nothing points at it.
        mMaterialScratch.push_back(Shaders::GpuMaterial{
            .mKind = Shaders::KIND_SURFACE,
            .mDiffuse = Shaders::NO_TEXTURE,
            .mAlphaCutoff = 0.0f,
            .mLayerOffset = 0,
            .mLayerCount = 0,
            .mEmissive = Shaders::NO_TEXTURE,
            .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f),
            .mEmissiveColour = osg::Vec3f(0.0f, 0.0f, 0.0f),
            .mTextureTransform = osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f),
        });

        mLayerScratch.clear();
        mLayerScratch.reserve(scene.getLayers().size());
        for (const MaterialLayer& layer : scene.getLayers())
            mLayerScratch.push_back(toGpu(layer));

        // A scene with no terrain in it still has to bind something: a descriptor may not be null,
        // and a zero-length buffer is not a thing Vulkan will make. One unread element each — and
        // the layer cannot be `constexpr`, because `osg::Vec4f` has no constexpr default.
        const Shaders::GpuLayer noLayer{};
        constexpr float noMask = 1.0f;

        const std::span<const Shaders::GpuMaterial> materials(mMaterialScratch);
        const std::span<const Shaders::GpuLayer> layers = mLayerScratch.empty()
            ? std::span<const Shaders::GpuLayer>(&noLayer, 1)
            : std::span<const Shaders::GpuLayer>(mLayerScratch);
        const std::span<const float> masks
            = scene.getMasks().empty() ? std::span<const float>(&noMask, 1) : scene.getMasks();

        reserve(mMaterials, materials.size_bytes());
        reserve(mLayers, layers.size_bytes());
        reserve(mMasks, masks.size_bytes());

        mMaterials.write(materials);
        mLayers.write(layers);
        mMasks.write(masks);
    }

    void SceneBuffers::place(const SceneDesc& scene, std::span<const InstanceRecord> records, const SeaState& sea)
    {
        shade(scene);

        // The sentinel material sits one past the real ones, which is where the constructor put it.
        const auto sentinel = static_cast<std::uint32_t>(scene.getMaterials().size());

        // **Indexed by slot, gaps included.** A hit reads its slot back as the custom index and
        // looks the row up here directly, so a table that closed its gaps would answer for the
        // wrong placement.
        //
        // Resized rather than reassigned: a gap's row is never read, so filling the whole table
        // with zeroes before writing the rows over them is a second pass across three megabytes a
        // frame that nothing needs.
        const std::span<const MeshInstance> placements = scene.getInstances();
        mInstanceScratch.resize(records.size());

        for (std::size_t slot = 0; slot < records.size(); ++slot)
        {
            const InstanceRecord& record = records[slot];
            if (!record.mPlaced)
                continue;

            Shaders::GpuInstance& row = mInstanceScratch[slot];
            row.mMesh = record.mMesh;
            row.mMaterial = placements[slot].mMaterial == sNoIndex ? sentinel : placements[slot].mMaterial;

            for (int r = 0; r < 3; ++r)
                row.mMotion[r] = osg::Vec4f(record.mMotion.mRows[r][0], record.mMotion.mRows[r][1],
                    record.mMotion.mRows[r][2], record.mMotion.mRows[r][3]);
        }

        mLightScratch.clear();
        mLightScratch.reserve(scene.getLights().size());
        for (const Light& light : scene.getLights())
            mLightScratch.push_back(toGpu(light));

        mSpriteScratch.clear();
        mSpriteScratch.reserve(scene.getSprites().size());
        for (const Sprite& sprite : scene.getSprites())
            mSpriteScratch.push_back(toGpu(sprite));

        mEmitterScratch.clear();
        mEmitterScratch.reserve(scene.getEmitters().size());
        for (const SpriteEmitter& emitter : scene.getEmitters())
            mEmitterScratch.push_back(toGpu(emitter));

        // **Which emitter placed a sprite, written from this side because only this side knows.**
        // The scene keeps the pairing as a run on the emitter; a tile's list is sprites, and a
        // sprite walked out of one has to be able to say when the run it belongs to has changed.
        for (std::uint32_t at = 0; at < mEmitterScratch.size(); ++at)
        {
            const Shaders::GpuEmitter& emitter = mEmitterScratch[at];
            for (std::uint32_t sprite = emitter.mFirst; sprite < emitter.mFirst + emitter.mCount; ++sprite)
                mSpriteScratch[sprite].mEmitter = at;
        }

        mLightGrid.rebuild(scene.getLights());

        // **The tables go over as they are, empty ones included.** Something has to be bound to a
        // descriptor the shader declares, and `growTo` is what guarantees it — each of these used to
        // carry a one-element stand-in of its own to say the same thing, five of them, and the one
        // table that had none is what cost a device. What stops the shader reading an empty table is
        // its count, exactly as it always was.
        const std::span<const Shaders::GpuInstance> instances(mInstanceScratch);
        const std::span<const Shaders::GpuLight> lights(mLightScratch);
        const std::span<const std::uint32_t> indices = mLightGrid.getIndices();
        const std::span<const Shaders::GpuSprite> sprites(mSpriteScratch);
        const std::span<const Shaders::GpuEmitter> emitters(mEmitterScratch);

        const Shaders::GpuLightGrid geometry{
            .mOrigin = mLightGrid.getOrigin(),
            .mInverseCell = mLightGrid.getInverseCell(),
            .mSize = mLightGrid.getSize(),
        };
        const std::array<Shaders::GpuWave, Shaders::WAVE_COUNT> waves = sea.getWaves();

        reserve(mInstances, instances.size_bytes());
        reserve(mLights, lights.size_bytes());
        reserve(mLightOffsets, mLightGrid.getOffsets().size_bytes());
        reserve(mLightIndices, indices.size_bytes());
        reserve(mGrid, sizeof(geometry));
        reserve(mWaves, sizeof(waves));
        reserve(mSprites, sprites.size_bytes());
        reserve(mEmitters, emitters.size_bytes());

        mInstances.write(instances);
        mLights.write(lights);
        mLightOffsets.write(mLightGrid.getOffsets());
        mLightIndices.write(indices);
        mGrid.write(std::span<const Shaders::GpuLightGrid>(&geometry, 1));
        mWaves.write(std::span<const Shaders::GpuWave>(waves));
        mSprites.write(sprites);
        mEmitters.write(emitters);

        // **Only what changed shape.** A cell's normals are the same normals from one frame to the
        // next; a skinned body's are new every frame, and `getDeformed` is the list of exactly those.
        for (const Index mesh : scene.getDeformed())
        {
            const MeshRange& range = scene.getMeshes()[mesh];
            mNormals.writeAt(range.mVertexOffset, scene.getNormals().subspan(range.mVertexOffset, range.mVertexCount));
        }
    }

    VkDeviceSize SceneBuffers::getBytes() const
    {
        // The indices are not counted here: they belong to the acceleration structure, which reports
        // its own size.
        return mNormals.getBytes() + mTexCoords.getBytes() + mMeshes.getSize() + mInstances.getSize()
            + mMaterials.getSize() + mLayers.getSize() + mMasks.getSize() + mLights.getSize() + mLightOffsets.getSize()
            + mLightIndices.getSize() + mGrid.getSize() + mWaves.getSize() + mSprites.getSize() + mEmitters.getSize();
    }
}
