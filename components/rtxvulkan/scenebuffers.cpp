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
        // Addressable and never bound: the frame block carries where every table is, and no
        // descriptor names one.
        constexpr VkBufferUsageFlags sTableUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        Shaders::GpuMaterial toGpu(const Material& material)
        {
            // Zero where the material has no texture to read a mask out of, so that the shader's
            // comparison agrees with `Material::isCutout`, which is what decided whether traversal
            // would ever make it.
            return Shaders::GpuMaterial{
                .mDiffuse = material.mDiffuse,
                .mAlphaCutoff = material.isCutout() ? material.getAlphaCutoff() : 0.0f,

                // One where the surface is all there, so traversal branches on a number rather than
                // on a mode it was never sent.
                .mOpacity = material.isTranslucent() ? material.mDiffuseColour.a() : 1.0f,
                .mLayerOffset = material.mLayerOffset,
                .mLayerCount = material.mLayerCount,
                .mEmissive = material.mEmissive,
                .mDiffuseColour
                = osg::Vec3f(material.mDiffuseColour.r(), material.mDiffuseColour.g(), material.mDiffuseColour.b()),
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
                .mSourceRadius = light.mSourceRadius,
                .mClearance = light.mClearance,
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
                .mDiffuse = Shaders::NO_TEXTURE,
                .mAlphaCutoff = 0.0f,
                .mOpacity = 1.0f,
                .mLayerOffset = 0,
                .mLayerCount = 0,
                .mEmissive = Shaders::NO_TEXTURE,
                .mDiffuseColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
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
        mInstanceTable.open(device, slots, sTableUsage, "instance rows");
        mMaterialTable.open(device, slots, sTableUsage, "materials");
        mNormalTable.open(device, slots, sTableUsage, "normals");

        // **Every table exists from here, whether or not anything has been written to it.** A frame
        // carries the address of all of them and the shader reaches all of them; what fills one is a
        // later call that a frame may never make — a scene with no sprites never bins any, and the
        // tiles were then bound as nothing at all. Growing on write cannot carry that guarantee,
        // because the write is exactly what does not happen.
        graveyard.bury(growTo(mMeshes, device, 0, sTableUsage));
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
        {
            Tables& tables = mTables[slot];

            for (Buffer* table : { &tables.mLayers, &tables.mMasks, &tables.mLights, &tables.mLightList,
                     &tables.mSprites, &tables.mEmitters, &tables.mSpriteTileList })
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
            mNormalTable.settle(slot);

        // The frame tables come from `place`, which is also where they are written when a material
        // changes. Every copy is empty here, so the first write of each makes its buffer and fills
        // it whole.
        place(scene, records, {}, 0, graveyard);
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
        mNormalTable.reserve(static_cast<std::uint32_t>(scene.getNormals().size()));

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.getMeshes()[mesh];
            if (range.mVertexCount == 0)
                continue;

            const std::span<const osg::Vec3f> normals
                = scene.getNormals().subspan(range.mVertexOffset, range.mVertexCount);
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mNormalTable.at(slot).writeAt(range.mVertexOffset, normals);

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
                .mShape
                = (mesh.mShape.mSheet ? Shaders::MESH_SHEET : 0u) | (mesh.mShape.mClosed ? Shaders::MESH_CLOSED : 0u),
            });

        reserve(mMeshes, mMeshScratch.size() * sizeof(Shaders::GpuMesh), graveyard);
        mMeshes.write(std::span<const Shaders::GpuMesh>(mMeshScratch));
        mDevice->setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(mMeshes.getHandle()), "meshes");
    }

    void SceneBuffers::reserve(Buffer& held, const VkDeviceSize bytes, Graveyard& graveyard)
    {
        graveyard.bury(growTo(held, *mDevice, bytes, sTableUsage));
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

        // A frame with no sprites has a list that is all starts and no runs, so the shader reads none
        // of it past the start it looks up.
        const std::span<const std::uint32_t> tileList = mSpriteTiles.getList().getWhole();
        reserve(tables.mSpriteTileList, tileList.size_bytes(), graveyard);
        tables.mSpriteTileList.write(tileList);
    }

    void SceneBuffers::shade(const SceneDesc& scene, const std::uint32_t slot, Graveyard& graveyard)
    {
        const std::span<const Material> materials = scene.getMaterials();
        const std::span<const MaterialLayer> layers = scene.getLayers();
        const std::span<const float> masks = scene.getMasks();

        // **Every row where the table changed length, and the rows the scene wrote otherwise.** The
        // sentinel sits one past the real materials, so a table that grew has a real material where
        // the sentinel was and the sentinel where nothing was — two rows to reason about separately,
        // or every row written on a path only a cell arrival takes. A material is sixty-eight bytes.
        const bool moved = mMaterialTable.size() != materials.size() + 1;
        mMaterialTable.resize(materials.size() + 1);

        if (moved)
        {
            for (std::size_t at = 0; at < materials.size(); ++at)
                mMaterialTable.write(static_cast<Index>(at)) = toGpu(materials[at]);

            mMaterialTable.write(static_cast<Index>(materials.size())) = sentinelMaterial();
        }
        else
        {
            // A material a flipbook rewrote is one row; the table around it is what it was.
            for (const Index at : scene.getWrittenMaterials())
                mMaterialTable.write(at) = toGpu(materials[at]);
        }

        mMaterialTable.sync(slot, graveyard);

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

            if (outgrow(copy.mLayers, *mDevice, std::max<std::size_t>(layers.size(), 1) * sizeof(Shaders::GpuLayer),
                    sTableUsage, graveyard))
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

            if (outgrow(copy.mMasks, *mDevice, std::max<std::size_t>(masks.size(), 1) * sizeof(float), sTableUsage,
                    graveyard))
                copy.mMasks.write(masks.empty() ? std::span<const float>(&noMask, 1) : masks);
            else
                for (const Span run : scene.getArrivedMasks())
                    copy.mMasks.writeAt(run.mOffset * sizeof(float), masks.subspan(run.mOffset, run.mCount));
        }
    }

    void SceneBuffers::place(const SceneDesc& scene, std::span<const InstanceRecord> records,
        std::span<const Index> changed, const std::uint32_t slot, Graveyard& graveyard)
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

        const std::size_t had = mInstanceTable.size();
        mInstanceTable.resize(records.size());

        const auto placeRow = [&](const std::size_t at) {
            const InstanceRecord& record = records[at];
            if (!record.mPlaced)
                return;

            Shaders::GpuInstance& row = mInstanceTable.write(static_cast<Index>(at));
            row.mMesh = record.mMesh;
            row.mMaterial = placements[at].mMaterial == sNoIndex ? sentinel : placements[at].mMaterial;
            row.mOpacity = placements[at].mOpacity;

            for (int r = 0; r < 3; ++r)
                row.mMotion[r] = osg::Vec4f(record.mMotion.mRows[r][0], record.mMotion.mRows[r][1],
                    record.mMotion.mRows[r][2], record.mMotion.mRows[r][3]);
        };

        // **The rows this placement wrote, and whatever the table grew by.** A world is tens of
        // thousands of placements and a frame moves hundreds; writing every row to change those was
        // a memcpy of megabytes a frame. Which copies are then behind is the table's own answer,
        // and it is the same answer the acceleration structure's rows get from the same list.
        for (std::size_t at = had; at < records.size(); ++at)
            placeRow(at);

        for (const Index at : changed)
            placeRow(at);

        mInstanceTable.sync(slot, graveyard);

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

        // **The tables go over as they are, empty ones included.** Something has to stand at every
        // address the frame carries, and `growTo` is what guarantees it — each of these used to
        // carry a one-element stand-in of its own to say the same thing, five of them, and the one
        // table that had none is what cost a device. What stops the shader reading an empty table is
        // its count, exactly as it always was.
        const std::span<const Shaders::GpuLight> lights(mLightScratch);
        const std::span<const std::uint32_t> lightList = mLightGrid.getList().getWhole();
        const std::span<const Shaders::GpuEmitter> emitters(mEmitterScratch);

        reserve(tables.mLights, lights.size_bytes(), graveyard);
        reserve(tables.mLightList, lightList.size_bytes(), graveyard);
        reserve(tables.mEmitters, emitters.size_bytes(), graveyard);

        tables.mLights.write(lights);
        tables.mLightList.write(lightList);
        tables.mEmitters.write(emitters);

        // The normals of anything skinned are not written here: a cell's are the same from one
        // frame to the next, and a body's are what `SkinPass` computed into this copy ahead of this.
    }

    void SceneBuffers::describeTables(const std::uint32_t slot, Shaders::GpuTables& into) const
    {
        assert(slot < mSlots && "a frame slot this scene has no copy of the tables for");

        const Tables& tables = mTables[slot];

        into.mNormalBlocks = mNormalTable.at(slot).getTableAddress();
        into.mTexCoordBlocks = mTexCoords.getTableAddress();
        into.mMeshes = mMeshes.getDeviceAddress();
        into.mInstances = mInstanceTable.getDeviceAddress(slot);
        into.mMaterials = mMaterialTable.getDeviceAddress(slot);
        into.mLayers = tables.mLayers.getDeviceAddress();
        into.mMasks = tables.mMasks.getDeviceAddress();
        into.mLights = tables.mLights.getDeviceAddress();
        into.mLightList = tables.mLightList.getDeviceAddress();
        into.mSprites = tables.mSprites.getDeviceAddress();
        into.mEmitters = tables.mEmitters.getDeviceAddress();
        into.mSpriteTileList = tables.mSpriteTileList.getDeviceAddress();
    }

    VkDeviceSize SceneBuffers::Tables::getBytes() const
    {
        return mLayers.getSize() + mMasks.getSize() + mLights.getSize() + mLightList.getSize() + mSprites.getSize()
            + mEmitters.getSize() + mSpriteTileList.getSize();
    }

    VkDeviceSize SceneBuffers::getBytes() const
    {
        // The indices are not counted here: they belong to the acceleration structure, which reports
        // its own size.
        VkDeviceSize total = mTexCoords.getBytes() + mMeshes.getSize() + mInstanceTable.getBytes()
            + mMaterialTable.getBytes() + mNormalTable.getBytes();
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
            total += mTables[slot].getBytes();

        return total;
    }
}
