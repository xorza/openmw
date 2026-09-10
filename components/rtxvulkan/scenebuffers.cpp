#include "scenebuffers.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenetables.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/surface/vertexcolour.hpp>

#include "commands.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "spritebinpass.hpp"
#include "spriteshadepass.hpp"

namespace Rtx
{
    namespace
    {
        // Addressable and never bound: the frame block carries where every table is, and no
        // descriptor names one.
        constexpr VkBufferUsageFlags sTableUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        // The sprite tiles' list is the one table the device fills, and its head is zeroed by a
        // fill before every bin.
        constexpr VkBufferUsageFlags sSpriteListUsage
            = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        /// What a material's vertex-colour mode is worth to the shader: one bit, or none.
        ///
        /// The mode does not survive the trip, for the reason `GpuMaterial::mAlphaCutoff` gives:
        /// what the shader does is a `mix` against a weight, and the host is what settles which of
        /// the two colours the weight picks.
        std::uint32_t vertexColourFlag(const Surface::VertexColour colour)
        {
            switch (colour)
            {
                case Surface::VertexColour::Tint:
                    return Shaders::MATERIAL_VERTEX_TINT;
                case Surface::VertexColour::Glow:
                    return Shaders::MATERIAL_VERTEX_GLOW;
                case Surface::VertexColour::None:
                    break;
            }

            return 0u;
        }

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
                .mLayerOffset = material.mLayers.mOffset,
                .mLayerCount = material.mLayers.mCount,
                .mEmissive = material.mEmissive,
                .mDiffuseColour
                = osg::Vec3f(material.mDiffuseColour.r(), material.mDiffuseColour.g(), material.mDiffuseColour.b()),
                .mEmissiveColour = material.mEmissiveColour,
                .mTextureTransform = material.mTextureTransform,
                .mFlags
                = (material.isMedium() ? Shaders::MATERIAL_MEDIUM : 0u) | vertexColourFlag(material.mVertexColour),
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
                .mAxis = sprite.mAxis,
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
                .mFirst = emitter.mSprites.mOffset,
                .mCount = emitter.mSprites.mCount,
                .mTexture = emitter.mTexture,
                .mAdditive = emitter.mAdditive ? 1u : 0u,
                .mWidth = emitter.mWidth,
                .mLighting = emitter.mLighting,
            };
        }

        Shaders::GpuLayer toGpu(const MaterialLayer& layer)
        {
            return Shaders::GpuLayer{
                .mDiffuse = layer.mDiffuse,
                .mMaskOffset = layer.mMask.mOffset,
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

    SceneBuffers::SceneBuffers(const Device& device, Batch& batch, const SceneTables& scene,
        std::span<const InstanceRecord> records, const std::uint32_t slots, Graveyard& graveyard)
        : mDevice(&device)
        , mSlots(slots)
    {
        assert(slots >= 1 && slots <= sFrameSlots && "more frames in flight than there are copies of the tables");

        mTexCoords.open(device, sTableUsage, "uvs");
        mColours.open(device, sTableUsage, "vertex colours");
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
                     &tables.mSprites, &tables.mEmitters, &tables.mSpriteRects })
                graveyard.bury(growTo(*table, device, 0, sTableUsage));

            graveyard.bury(growTo(tables.mSpriteTileList, device, 0, sSpriteListUsage));

            // Read before it is first written, so it has to say that nothing was needed yet.
            tables.mSpriteBinReport
                = Buffer::staging(device, sizeof(std::uint32_t), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            *static_cast<std::uint32_t*>(tables.mSpriteBinReport.map()) = 0;
        }

        // Every mesh the scene holds, which is the same path an arrival takes with a shorter list.
        std::vector<Index> every(scene.mMeshes.getRows().size());
        for (std::size_t at = 0; at < every.size(); ++at)
            every[at] = static_cast<Index>(at);

        writeMeshes(batch, scene, every, graveyard);

        // Every copy of the normals holds every mesh from here, so what a copy owes from now on is
        // the poses it missed.
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
            mNormalTable.settle(FrameSlot{ slot });

        // The frame tables come from `place`, which is also where they are written when a material
        // changes. Every copy is empty here, so the first write of each makes its buffer and fills
        // it whole.
        place(scene, records, {}, FrameSlot{}, graveyard);
    }

    void SceneBuffers::extend(Batch& batch, const SceneTables& scene, Graveyard& graveyard)
    {
        writeMeshes(batch, scene, scene.mMeshes.getArrived(), graveyard);
    }

    void SceneBuffers::writeMeshes(
        Batch& batch, const SceneTables& scene, std::span<const Index> meshes, Graveyard& graveyard)
    {
        // **Whole runs here and a mesh at a time afterwards.** Only a skinned body's normals change,
        // so filling these when the mesh arrives is a load's cost and every frame after it pays for
        // what actually moved.
        mTexCoords.reserve(batch, static_cast<std::uint32_t>(scene.mMeshes.getTexCoords().size()));
        mColours.reserve(batch, static_cast<std::uint32_t>(scene.mMeshes.getColours().size()));
        mNormalTable.reserve(batch, static_cast<std::uint32_t>(scene.mMeshes.getNormals().size()));

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.mMeshes.getRows()[mesh];
            if (range.mVertices.empty())
                continue;

            const std::span<const osg::Vec3f> normals = range.mVertices.in(scene.mMeshes.getNormals());
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mNormalTable.at(FrameSlot{ slot }).writeAt(batch, range.mVertices.mOffset, normals);

            mTexCoords.writeAt(batch, range.mVertices.mOffset, range.mVertices.in(scene.mMeshes.getTexCoords()));
            mColours.writeAt(batch, range.mVertices.mOffset, range.mVertices.in(scene.mMeshes.getColours()));
        }

        // **What is built out of these was copied a moment ago.** The blocks are device memory, so a
        // mesh reaches them through a transfer rather than through a host write that a submit already
        // orders — and the acceleration structures built from them are recorded into this same
        // command buffer. One dependency for every block, because they are read together.
        orderStagedWrites(batch);

        // **Whole, and it is twelve bytes a slot.** A mesh arriving moves nothing already in this,
        // but sizing it to the scene means growing it, and growing means writing it — so the rows
        // that did not change are written again for the price of not having to know which did.
        mMeshScratch.clear();
        mMeshScratch.reserve(scene.mMeshes.getRows().size());
        for (const MeshRange& mesh : scene.mMeshes.getRows())
            mMeshScratch.push_back(Shaders::GpuMesh{
                .mVertexOffset = mesh.mVertices.mOffset,
                .mIndexOffset = mesh.mIndices.mOffset,
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

    void SceneBuffers::binSprites(const SpriteShadePass& shading, const SpriteBinPass& pass, const osg::Vec3f& origin,
        const Shaders::Camera& camera, const osg::Vec3f& toSun, const Placing& placing)
    {
        assert(placing.mSlot.get() < mSlots && "a frame slot this scene has no copy of the tables for");

        Tables& tables = mTables[placing.mSlot.get()];

        // **The sprites go over from here and not from `place`**, because what each is shaded by is
        // the frame's sun, which a placement does not know — and a doll or a map bins against a
        // camera and a sun of its own.
        const std::span<const Shaders::GpuSprite> sprites(mSpriteScratch);
        reserve(tables.mSprites, sprites.size_bytes(), placing.mGraveyard);
        tables.mSprites.write(sprites);

        const auto count = static_cast<std::uint32_t>(sprites.size());

        // **Before the bin and after the write**, because the bin reads a sprite's position and the
        // trace reads its layers, and both read the table this fills in. The sprites go over
        // unshaded and come back shaded in place.
        //
        // Scratch for the two depth orders, one key a sprite a light. Nothing reads it after the
        // dispatch and nothing carries it between frames, so it is sized and forgotten.
        reserve(tables.mSpriteOrder, VkDeviceSize{ count } * Shaders::SPRITE_SHADE_LIGHTS * sizeof(std::uint64_t),
            placing.mGraveyard);

        shading.record(placing.mCommands,
            Shaders::SpriteShadeConstants{
                .mSprites = tables.mSprites.getDeviceAddress(),
                .mEmitters = tables.mEmitters.getDeviceAddress(),
                .mOrder = tables.mSpriteOrder.getDeviceAddress(),
                .mToSun = toSun,
                .mEmitterCount = static_cast<std::uint32_t>(mEmitterScratch.size()),
                .mCount = count,
            },
            placing.mTimer);

        // **Sized from what this copy's last bin said it needed, with room over it**, because the
        // need is only known once the tiles are counted and that happens on the device. The fence
        // this copy's last frame signalled is what makes the report readable here.
        // `SpriteListSize` says the rest of the policy and why one object holds it.
        const std::uint32_t reported = *static_cast<const std::uint32_t*>(tables.mSpriteBinReport.map());
        tables.mSpriteListSize.sizeFor(Shaders::spriteTilesIn(camera.mWidth, camera.mHeight), count, reported);

        placing.mGraveyard.bury(
            growTo(tables.mSpriteTileList, *mDevice, tables.mSpriteListSize.getBytes(), sSpriteListUsage));
        reserve(tables.mSpriteRects, VkDeviceSize{ count } * sizeof(std::uint64_t), placing.mGraveyard);

        pass.record(placing.mCommands,
            Shaders::SpriteBinConstants{
                .mSprites = tables.mSprites.getDeviceAddress(),
                .mEmitters = tables.mEmitters.getDeviceAddress(),
                .mRects = tables.mSpriteRects.getDeviceAddress(),
                .mList = tables.mSpriteTileList.getDeviceAddress(),
                .mReport = tables.mSpriteBinReport.getDeviceAddress(),
                .mOrigin = origin,
                .mCamera = camera,
                .mCount = count,
                .mCapacity = tables.mSpriteListSize.getCapacity(),
            },
            tables.mSpriteTileList, placing.mTimer);

        // **What the next bin of this copy sizes its list from**, read on the host once the frame's
        // fence has been waited on. A fence's access scope is the device's, so without this the
        // figure is whatever the caches held.
        tables.mSpriteBinReport.orderForHostRead(placing.mCommands);
    }

    void SceneBuffers::shade(const SceneTables& scene, const FrameSlot slot, Graveyard& graveyard)
    {
        const std::span<const Material> materials = scene.mMaterials.getRows();
        const std::span<const MaterialLayer> layers = scene.mMaterials.getLayers();
        const std::span<const float> masks = scene.mMaterials.getMasks();

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
            for (const Index at : scene.mMaterials.getWritten())
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
                for (const Run run : scene.mMaterials.getArrived().mLayers)
                {
                    mLayerScratch.clear();
                    mLayerScratch.reserve(run.mCount);
                    for (const MaterialLayer& layer : run.in(layers))
                        mLayerScratch.push_back(toGpu(layer));

                    copy.mLayers.writeAt(
                        run.mOffset * sizeof(Shaders::GpuLayer), std::span<const Shaders::GpuLayer>(mLayerScratch));
                }
            }

            if (outgrow(copy.mMasks, *mDevice, std::max<std::size_t>(masks.size(), 1) * sizeof(float), sTableUsage,
                    graveyard))
                copy.mMasks.write(masks.empty() ? std::span<const float>(&noMask, 1) : masks);
            else
                for (const Run run : scene.mMaterials.getArrived().mMasks)
                    copy.mMasks.writeAt(run.mOffset * sizeof(float), run.in(masks));
        }
    }

    void SceneBuffers::place(const SceneTables& scene, std::span<const InstanceRecord> records,
        std::span<const Index> changed, const FrameSlot slot, Graveyard& graveyard)
    {
        assert(slot.get() < mSlots && "a frame slot this scene has no copy of the tables for");

        shade(scene, slot, graveyard);

        Tables& tables = mTables[slot.get()];

        // The sentinel material sits one past the real ones, which is where `shade` put it.
        const auto sentinel = static_cast<std::uint32_t>(scene.mMaterials.getRows().size());

        // **Indexed by slot, gaps included.** A hit reads its slot back as the custom index and
        // looks the row up here directly, so a table that closed its gaps would answer for the
        // wrong placement. A gap's row is never read, so it is never written either.
        const std::span<const MeshInstance> placements = scene.mPlacements.getAll();

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
        mLightScratch.reserve(scene.mLights.size());
        for (const Light& light : scene.mLights)
            mLightScratch.push_back(toGpu(light));

        mSpriteScratch.clear();
        mSpriteScratch.reserve(scene.mSprites.size());
        for (const Sprite& sprite : scene.mSprites)
            mSpriteScratch.push_back(toGpu(sprite));

        mEmitterScratch.clear();
        mEmitterScratch.reserve(scene.mEmitters.size());
        for (const SpriteEmitter& emitter : scene.mEmitters)
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

        mLightGrid.rebuild(scene.mLights);

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

    void SceneBuffers::describeTables(const FrameSlot slot, Shaders::GpuTables& into) const
    {
        assert(slot.get() < mSlots && "a frame slot this scene has no copy of the tables for");

        const Tables& tables = mTables[slot.get()];

        into.mNormalBlocks = mNormalTable.at(slot).getTableAddress();
        into.mTexCoordBlocks = mTexCoords.getTableAddress();
        into.mColourBlocks = mColours.getTableAddress();
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
            + mEmitters.getSize() + mSpriteTileList.getSize() + mSpriteRects.getSize() + mSpriteBinReport.getSize();
    }

    VkDeviceSize SceneBuffers::getBytes() const
    {
        // The indices are not counted here: they belong to the acceleration structure, which reports
        // its own size.
        VkDeviceSize total = mTexCoords.getBytes() + mColours.getBytes() + mMeshes.getSize() + mInstanceTable.getBytes()
            + mMaterialTable.getBytes() + mNormalTable.getBytes();
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
            total += mTables[slot].getBytes();

        return total;
    }
}
