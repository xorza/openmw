#include "scenebuffers.hpp"

#include <algorithm>
#include <cassert>
#include <string>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>

#include "device.hpp"
#include "graveyard.hpp"

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

                // One where the surface is all there, so traversal branches on a number rather than
                // on a mode it was never sent.
                .mOpacity = material.isTranslucent() ? material.mDiffuseColour.a() : 1.0f,
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
                .mLighting = emitter.mLighting,
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

        /// A drawable with no state set has no material, and `sNoIndex` is not somewhere the shader
        /// can be allowed to look. One untextured entry past the table costs less than a branch per
        /// hit, and every instance that had nothing points at it. It moves when the table grows,
        /// which is why the count it was last written at is kept per copy.
        Shaders::GpuMaterial sentinelMaterial()
        {
            return Shaders::GpuMaterial{
                .mKind = Shaders::KIND_SURFACE,
                .mDiffuse = Shaders::NO_TEXTURE,
                .mAlphaCutoff = 0.0f,
                .mOpacity = 1.0f,
                .mLayerOffset = 0,
                .mLayerCount = 0,
                .mEmissive = Shaders::NO_TEXTURE,
                .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f),
                .mEmissiveColour = osg::Vec3f(0.0f, 0.0f, 0.0f),
                .mTextureTransform = osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f),
            };
        }
    }

    SceneBuffers::SceneBuffers(const Device& device, const SceneDesc& scene, std::span<const InstanceRecord> records,
        const std::uint32_t slots, Graveyard& graveyard)
        : mDevice(&device)
        , mSlots(slots)
    {
        assert(slots >= 1 && slots <= sFrameSlots && "more frames in flight than there are copies of the tables");

        mTexCoords.open(device, sTableUsage, "uvs");

        // **Every table exists from here, whether or not anything has been written to it.** A pass
        // binds all of them and the shader declares all of them; what fills one is a later call that
        // a frame may never make — a scene with no sprites never bins any, and the tiles were then
        // bound as nothing at all. Growing on write cannot carry that guarantee, because the write is
        // exactly what does not happen.
        graveyard.bury(growTo(mMeshes, device, 0, sTableUsage));
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
        {
            Tables& tables = mTables[slot];
            tables.mNormals.open(device, sTableUsage, "normals");

            for (HostBuffer* table : { &tables.mInstances, &tables.mMaterials, &tables.mLayers, &tables.mMasks,
                     &tables.mLights, &tables.mLightOffsets, &tables.mGrid, &tables.mLightIndices, &tables.mSprites,
                     &tables.mEmitters, &tables.mSpriteTileOffsets, &tables.mSpriteTileIndices })
                graveyard.bury(growTo(*table, device, 0, sTableUsage));
        }

        // Every mesh the scene holds, which is the same path an arrival takes with a shorter list.
        std::vector<Index> every(scene.getMeshes().size());
        for (std::size_t at = 0; at < every.size(); ++at)
            every[at] = static_cast<Index>(at);

        writeMeshes(scene, every, graveyard);

        // Every copy of the normals holds every mesh from here, so what a copy owes from now on is
        // the poses it missed.
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
            mTables[slot].mNormalsOwed.settle();

        // The frame tables come from `place`, which is also where they are written when a material
        // changes. Every one of them is a byte long here, so the first write of each copy makes it
        // again and fills it whole.
        place(scene, records, 0, graveyard);

        device.setName(
            VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(mTables[0].mInstances.getHandle()), "instance rows");
    }

    void SceneBuffers::extend(const SceneDesc& scene, Graveyard& graveyard)
    {
        writeMeshes(scene, scene.getArrivedMeshes(), graveyard);
    }

    void SceneBuffers::writeMeshes(const SceneDesc& scene, std::span<const Index> meshes, Graveyard& graveyard)
    {
        // **Whole runs here and a mesh at a time afterwards.** Only a skinned body's normals change,
        // so filling these when the mesh arrives is a load's cost and every frame after it pays for
        // what actually moved.
        mTexCoords.reserve(static_cast<std::uint32_t>(scene.getTexCoords().size()));
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
            mTables[slot].mNormals.reserve(static_cast<std::uint32_t>(scene.getNormals().size()));

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.getMeshes()[mesh];
            if (range.mVertexCount == 0)
                continue;

            const std::span<const osg::Vec3f> normals
                = scene.getNormals().subspan(range.mVertexOffset, range.mVertexCount);
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mTables[slot].mNormals.writeAt(range.mVertexOffset, normals);

            mTexCoords.writeAt(
                range.mVertexOffset, scene.getTexCoords().subspan(range.mVertexOffset, range.mVertexCount));
        }

        // **Whole, and it is twelve bytes a slot.** A mesh arriving moves nothing already in this,
        // but sizing it to the scene means growing it, and growing means writing it — so the rows
        // that did not change are written again for the price of not having to know which did.
        mMeshScratch.clear();
        mMeshScratch.reserve(scene.getMeshes().size());
        for (const MeshRange& mesh : scene.getMeshes())
            mMeshScratch.push_back(Shaders::GpuMesh{
                .mVertexOffset = mesh.mVertexOffset,
                .mIndexOffset = mesh.mIndexOffset,
                .mSheet = mesh.mSheet ? 1u : 0u,
            });

        reserve(mMeshes, mMeshScratch.size() * sizeof(Shaders::GpuMesh), graveyard);
        mMeshes.write(std::span<const Shaders::GpuMesh>(mMeshScratch));
        mDevice->setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(mMeshes.getHandle()), "meshes");
    }

    void SceneBuffers::reserve(HostBuffer& held, const VkDeviceSize bytes, Graveyard& graveyard)
    {
        graveyard.bury(growTo(held, *mDevice, bytes, sTableUsage));
    }

    bool SceneBuffers::outgrow(HostBuffer& held, const VkDeviceSize bytes, Graveyard& graveyard)
    {
        if (held.getSize() >= bytes)
            return false;

        graveyard.bury(growTo(held, *mDevice, std::max(bytes, held.getSize() * 2), sTableUsage));
        return true;
    }

    void SceneBuffers::binSprites(const osg::Vec3f& origin, const Shaders::Camera& camera, const osg::Vec3f& toSun,
        const std::uint32_t slot, Graveyard& graveyard)
    {
        Tables& tables = mTables[slot];

        // **The sprites go over from here and not from `place`**, because what each is shaded by is
        // the frame's sun, which a placement does not know — and a doll or a map bins against a
        // camera and a sun of its own.
        mSpriteShade.shade(mSpriteScratch, mEmitterScratch, toSun);

        const std::span<const Shaders::GpuSprite> sprites(mSpriteScratch);
        reserve(tables.mSprites, sprites.size_bytes(), graveyard);
        tables.mSprites.write(sprites);

        mSpriteTiles.rebuild(mSpriteScratch, mEmitterScratch, origin, camera);

        // A frame with no sprites has an empty list, and the offsets are all nought — so the shader
        // reads none of this and `growTo` is what makes sure there is something for it not to read.
        reserve(tables.mSpriteTileOffsets, mSpriteTiles.getOffsets().size_bytes(), graveyard);
        reserve(tables.mSpriteTileIndices, mSpriteTiles.getIndices().size_bytes(), graveyard);

        tables.mSpriteTileOffsets.write(mSpriteTiles.getOffsets());
        tables.mSpriteTileIndices.write(mSpriteTiles.getIndices());
    }

    void SceneBuffers::shade(const SceneDesc& scene, const std::uint32_t slot, Graveyard& graveyard)
    {
        const std::span<const Material> materials = scene.getMaterials();
        const std::span<const MaterialLayer> layers = scene.getLayers();
        const std::span<const float> masks = scene.getMasks();

        // Every copy owes the rows this frame wrote; this one pays now, the others when their frame
        // comes round.
        for (std::uint32_t each = 0; each < mSlots; ++each)
            mTables[each].mMaterialsOwed.owe(scene.getWrittenMaterials());

        Tables& tables = mTables[slot];
        RowDebt& owed = tables.mMaterialsOwed;
        const Shaders::GpuMaterial sentinel = sentinelMaterial();

        if (outgrow(tables.mMaterials, (materials.size() + 1) * sizeof(Shaders::GpuMaterial), graveyard)
            || owed.mEverything)
        {
            mMaterialScratch.clear();
            mMaterialScratch.reserve(materials.size() + 1);
            for (const Material& material : materials)
                mMaterialScratch.push_back(toGpu(material));
            mMaterialScratch.push_back(sentinel);

            tables.mMaterials.write(std::span<const Shaders::GpuMaterial>(mMaterialScratch));
        }
        else
        {
            // **The rows this copy owes, and the sentinel where the table it sits past grew.** A
            // material a flipbook rewrote is one row of eighty bytes; the table around it is what
            // it was.
            for (const Index at : owed.mRows)
            {
                const Shaders::GpuMaterial row = toGpu(materials[at]);
                tables.mMaterials.writeAt(at * sizeof(row), std::span<const Shaders::GpuMaterial>(&row, 1));
            }

            if (tables.mMaterialCount != materials.size())
                tables.mMaterials.writeAt(
                    materials.size() * sizeof(sentinel), std::span<const Shaders::GpuMaterial>(&sentinel, 1));
        }

        tables.mMaterialCount = materials.size();
        owed.settle();

        // A scene with no terrain in it still has to bind something: a descriptor may not be null,
        // and a zero-length buffer is not a thing Vulkan will make. One unread element each — and
        // the layer cannot be `constexpr`, because `osg::Vec4f` has no constexpr default.
        const Shaders::GpuLayer noLayer{};
        constexpr float noMask = 1.0f;

        // **Every copy, because a run only ever arrives with a chunk, and a chunk arriving is an
        // arrival the caller waited every frame out for.** Nothing is reading the other copies, so
        // they take the runs now rather than owing them; what a flipbook does every frame never
        // touches these tables.
        for (std::uint32_t each = 0; each < mSlots; ++each)
        {
            Tables& copy = mTables[each];

            if (outgrow(copy.mLayers, std::max<std::size_t>(layers.size(), 1) * sizeof(Shaders::GpuLayer), graveyard))
            {
                mLayerScratch.clear();
                mLayerScratch.reserve(layers.size());
                for (const MaterialLayer& layer : layers)
                    mLayerScratch.push_back(toGpu(layer));

                copy.mLayers.write(mLayerScratch.empty() ? std::span<const Shaders::GpuLayer>(&noLayer, 1)
                                                         : std::span<const Shaders::GpuLayer>(mLayerScratch));
            }
            else
            {
                // Each run as the chunk placed it: converted into the scratch and written at the
                // run's own offset, so a table of a thousand layers pays for the five that arrived.
                for (const Span run : scene.getArrivedLayers())
                {
                    mLayerScratch.clear();
                    mLayerScratch.reserve(run.mCount);
                    for (const MaterialLayer& layer : layers.subspan(run.mOffset, run.mCount))
                        mLayerScratch.push_back(toGpu(layer));

                    copy.mLayers.writeAt(
                        run.mOffset * sizeof(Shaders::GpuLayer), std::span<const Shaders::GpuLayer>(mLayerScratch));
                }
            }

            if (outgrow(copy.mMasks, std::max<std::size_t>(masks.size(), 1) * sizeof(float), graveyard))
                copy.mMasks.write(masks.empty() ? std::span<const float>(&noMask, 1) : masks);
            else
                for (const Span run : scene.getArrivedMasks())
                    copy.mMasks.writeAt(run.mOffset * sizeof(float), masks.subspan(run.mOffset, run.mCount));
        }
    }

    void SceneBuffers::place(
        const SceneDesc& scene, std::span<const InstanceRecord> records, const std::uint32_t slot, Graveyard& graveyard)
    {
        assert(slot < mSlots && "a frame slot this scene has no copy of the tables for");

        shade(scene, slot, graveyard);

        Tables& tables = mTables[slot];

        // The sentinel material sits one past the real ones, which is where `shade` put it.
        const auto sentinel = static_cast<std::uint32_t>(scene.getMaterials().size());

        // **Indexed by slot, gaps included.** A hit reads its slot back as the custom index and
        // looks the row up here directly, so a table that closed its gaps would answer for the
        // wrong placement. A gap's row is never read, so it is never written either.
        const std::span<const MeshInstance> placements = scene.getInstances();
        mInstanceRows.resize(records.size());

        const auto placeRow = [&](const std::size_t at) {
            const InstanceRecord& record = records[at];
            if (!record.mPlaced)
                return;

            Shaders::GpuInstance& row = mInstanceRows[at];
            row.mMesh = record.mMesh;
            row.mMaterial = placements[at].mMaterial == sNoIndex ? sentinel : placements[at].mMaterial;
            row.mOpacity = placements[at].mOpacity;

            for (int r = 0; r < 3; ++r)
                row.mMotion[r] = osg::Vec4f(record.mMotion.mRows[r][0], record.mMotion.mRows[r][1],
                    record.mMotion.mRows[r][2], record.mMotion.mRows[r][3]);
        };

        // **The rows the scene says changed, owed to every copy and paid by this one; the table
        // whole only where this copy was made again.** A world is tens of thousands of placements
        // and a frame moves hundreds; writing every row to change those was a memcpy of megabytes a
        // frame. What settled is a row too, because its motion went back to nothing and the row
        // still said otherwise.
        for (std::uint32_t each = 0; each < mSlots; ++each)
        {
            mTables[each].mRowsOwed.owe(scene.getSettled());
            mTables[each].mRowsOwed.owe(scene.getMoved());
        }

        RowDebt& owed = tables.mRowsOwed;
        if (outgrow(tables.mInstances, records.size() * sizeof(Shaders::GpuInstance), graveyard) || owed.mEverything)
        {
            for (std::size_t at = 0; at < records.size(); ++at)
                placeRow(at);

            tables.mInstances.write(std::span<const Shaders::GpuInstance>(mInstanceRows));
        }
        else
        {
            for (const Index at : owed.mRows)
            {
                placeRow(at);
                tables.mInstances.writeAt(
                    at * sizeof(Shaders::GpuInstance), std::span<const Shaders::GpuInstance>(&mInstanceRows[at], 1));
            }
        }
        owed.settle();

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
        const std::span<const Shaders::GpuLight> lights(mLightScratch);
        const std::span<const std::uint32_t> indices = mLightGrid.getIndices();
        const std::span<const Shaders::GpuEmitter> emitters(mEmitterScratch);

        const Shaders::GpuLightGrid geometry{
            .mOrigin = mLightGrid.getOrigin(),
            .mInverseCell = mLightGrid.getInverseCell(),
            .mSize = mLightGrid.getSize(),
        };
        reserve(tables.mLights, lights.size_bytes(), graveyard);
        reserve(tables.mLightOffsets, mLightGrid.getOffsets().size_bytes(), graveyard);
        reserve(tables.mLightIndices, indices.size_bytes(), graveyard);
        reserve(tables.mGrid, sizeof(geometry), graveyard);
        reserve(tables.mEmitters, emitters.size_bytes(), graveyard);

        tables.mLights.write(lights);
        tables.mLightOffsets.write(mLightGrid.getOffsets());
        tables.mLightIndices.write(indices);
        tables.mGrid.write(std::span<const Shaders::GpuLightGrid>(&geometry, 1));
        tables.mEmitters.write(emitters);

        // **Only what changed shape, and every pose this copy missed.** A cell's normals are the
        // same normals from one frame to the next; a skinned body's are new every frame, and
        // `getDeformed` is the list of exactly those — owed to every copy, because the copy the
        // frame after next reads was not written for this frame's pose.
        for (std::uint32_t each = 0; each < mSlots; ++each)
            mTables[each].mNormalsOwed.owe(scene.getDeformed());

        for (const Index mesh : tables.mNormalsOwed.mRows)
        {
            const MeshRange& range = scene.getMeshes()[mesh];
            tables.mNormals.writeAt(
                range.mVertexOffset, scene.getNormals().subspan(range.mVertexOffset, range.mVertexCount));
        }
        tables.mNormalsOwed.settle();
    }

    VkDeviceSize SceneBuffers::getBytes() const
    {
        // The indices are not counted here: they belong to the acceleration structure, which reports
        // its own size.
        VkDeviceSize total = mTexCoords.getBytes() + mMeshes.getSize();
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
        {
            const Tables& tables = mTables[slot];
            total += tables.mNormals.getBytes() + tables.mInstances.getSize() + tables.mMaterials.getSize()
                + tables.mLayers.getSize() + tables.mMasks.getSize() + tables.mLights.getSize()
                + tables.mLightOffsets.getSize() + tables.mLightIndices.getSize() + tables.mGrid.getSize()
                + tables.mSprites.getSize() + tables.mEmitters.getSize();
        }

        return total;
    }
}
