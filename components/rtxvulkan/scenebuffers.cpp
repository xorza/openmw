#include "scenebuffers.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/light.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/sprite.hpp>
#include <components/rtx/surface.hpp>

#include "bufferusage.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "spritepasses.hpp"
#include "timeline.hpp"

namespace Rtx
{
    namespace
    {
        /// What a material's vertex-colour mode is worth to the shader: one bit, or none. The
        /// shader does a `mix` against a weight, and the host settles which colour the weight picks.
        std::uint32_t vertexColourFlag(const VertexColour colour)
        {
            switch (colour)
            {
                case VertexColour::Tint:
                    return Shaders::MATERIAL_VERTEX_TINT;
                case VertexColour::Glow:
                    return Shaders::MATERIAL_VERTEX_GLOW;
                case VertexColour::None:
                    break;
            }

            return 0u;
        }

        Shaders::GpuMaterial toGpu(const Material& material)
        {
            // Zero where the material has no texture to read a mask out of, so that the shader's
            // comparison agrees with `Material::isCutout`, which is what decided whether traversal
            // would ever make it.
            // A material with no diffuse names the neutral slot, so the shader reads one path —
            // `TEXTURE_NEUTRAL` — and ground that kept its stack says so with a bit, which is the
            // one fact the sentinel was carrying.
            const bool untextured = material.mDiffuse == sNoIndex;

            return Shaders::GpuMaterial{
                .mDiffuse = untextured ? Shaders::TEXTURE_NEUTRAL : material.mDiffuse,
                .mAlphaCutoff = material.isCutout() ? material.getAlphaCutoff() : 0.0f,

                // One where the surface is all there, so traversal branches on a number rather than
                // on a mode it was never sent.
                //
                // **A blend and not a translucency.** `isTranslucent` asks the narrower question —
                // whether what stands behind shows through — which an additive sheet answers no to
                // while still weighing what it adds by that same alpha. Asked that way every
                // additive surface reaches the device at one, and a sheet an `NifOsg::AlphaController`
                // holds at nought stands at full strength: that alpha is what draws the rays around
                // the Heart of Lorkhan, and they would shine before the heart is struck.
                .mOpacity = material.isBlended() ? material.mOpacity : 1.0f,
                .mLayerOffset = material.mLayers.mOffset,
                .mLayerCount = material.mLayers.mCount,
                .mEmissive = material.mEmissive,
                .mDiffuseColour = material.mDiffuseColour,
                .mEmissiveColour = material.mEmissiveColour,
                .mTextureTransform = material.mTextureTransform,
                .mEnvironment = material.mEnvironment,
                .mEnvironmentColour = material.mEnvironmentColour,
                .mDark = material.mDark,
                .mFlags = (material.isMedium() ? Shaders::MATERIAL_MEDIUM : 0u)
                    | (untextured && material.mLayers.mCount > 0 ? Shaders::MATERIAL_STACKED : 0u)
                    | vertexColourFlag(material.mVertexColour)
                    | (material.isAdditive() && material.mBlend == BlendKind::AddWhole ? Shaders::MATERIAL_ADD_WHOLE
                                                                                       : 0u)
                    | ((material.mDarkUnit & Shaders::MATERIAL_DARK_UNIT_MASK) << Shaders::MATERIAL_DARK_UNIT_SHIFT),
            };
        }

        /// A drawable with no state set has no material, and `sNoIndex` is not somewhere the shader
        /// can be allowed to look. One untextured entry past the table costs less than a branch per
        /// hit, and every instance that had nothing points at it. It moves when the table grows,
        /// which is why the count it was last written at is kept per copy.
        Shaders::GpuMaterial sentinelMaterial()
        {
            return Shaders::GpuMaterial{
                .mDiffuse = Shaders::TEXTURE_NEUTRAL,
                .mAlphaCutoff = 0.0f,
                .mOpacity = 1.0f,
                .mLayerOffset = 0,
                .mLayerCount = 0,
                .mEmissive = Shaders::NO_TEXTURE,
                .mDiffuseColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mEmissiveColour = osg::Vec3f(0.0f, 0.0f, 0.0f),
                .mTextureTransform = osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f),
                .mEnvironment = Shaders::NO_TEXTURE,
                .mEnvironmentColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mDark = Shaders::NO_TEXTURE,
            };
        }
    }

    SceneBuffers::SceneBuffers(const Device& device, Batch& batch, const SceneDesc& scene,
        std::span<const InstanceRecord> records, const std::uint32_t slots)
        : mDevice(device)
    {
        mTables.open(slots);
        mTexCoords.open(device, sTableUsage, "uvs");
        mSecondTexCoords.open(device, sTableUsage, "second uvs");
        mColours.open(device, sTableUsage, "vertex colours");
        mInstanceTable.open(device, slots, sTableUsage, "instance rows");
        mMaterialTable.open(device, slots, sTableUsage, "materials");
        mNormalTable.open(device, slots, sTableUsage, "normals");
        mTangentTable.open(device, slots, sTableUsage, "tangents");

        // Every mesh the scene holds, which is the same path an arrival takes with a shorter list.
        std::vector<Index> every(scene.meshes().getRows().size());
        for (std::size_t at = 0; at < every.size(); ++at)
            every[at] = static_cast<Index>(at);

        writeMeshes(batch, scene, every);
        writeMaterialRuns(batch, scene);
        orderStagedWrites(batch);

        // Every copy of the normals and the tangents holds every mesh from here, so what a copy owes
        // from now on is the poses it missed.
        for (std::uint32_t slot = 0; slot < mNormalTable.count(); ++slot)
        {
            mNormalTable.settle(FrameSlot{ slot });
            mTangentTable.settle(FrameSlot{ slot });
        }

        // The frame tables come from `place`, which is also where they are written when a material
        // changes. Every copy is empty here, so the first write of each makes its buffer and fills
        // it whole.
        place(scene, records, {}, Placing{ .mSlot = FrameSlot{} });
    }

    void SceneBuffers::extend(Batch& batch, const SceneDesc& scene)
    {
        writeMeshes(batch, scene, scene.meshes().getArrived());
        writeMaterialRuns(batch, scene);

        // What is built out of the blocks was copied a moment ago, and the acceleration structures
        // built from them are recorded into this same command buffer. One dependency for every
        // block and every run, because they are read together.
        orderStagedWrites(batch);
    }

    void SceneBuffers::writeMeshes(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes)
    {
        // Whole runs here and a mesh at a time afterwards. Only a skinned body's normals change,
        // so filling these when the mesh arrives is a load's cost and every frame after it pays for
        // what actually moved.
        mTexCoords.reserve(batch, static_cast<std::uint32_t>(scene.meshes().getTexCoords().size()));
        mSecondTexCoords.reserve(batch, static_cast<std::uint32_t>(scene.meshes().getSecondTexCoords().size()));
        mColours.reserve(batch, static_cast<std::uint32_t>(scene.meshes().getColours().size()));
        mNormalTable.reserve(batch, static_cast<std::uint32_t>(scene.meshes().getNormals().size()));
        mTangentTable.reserve(batch, static_cast<std::uint32_t>(scene.meshes().getTangents().size()));

        for (const Index mesh : meshes)
        {
            const MeshRange& range = scene.meshes().getRows()[mesh];
            if (range.mVertices.empty())
                continue;

            const std::span<const osg::Vec3f> normals = range.mVertices.in(scene.meshes().getNormals());
            const std::span<const std::uint32_t> tangents = range.mVertices.in(scene.meshes().getTangents());
            for (std::uint32_t slot = 0; slot < mNormalTable.count(); ++slot)
            {
                mNormalTable.at(FrameSlot{ slot }).writeAt(batch, range.mVertices.mOffset, normals);
                mTangentTable.at(FrameSlot{ slot }).writeAt(batch, range.mVertices.mOffset, tangents);
            }

            mTexCoords.writeAt(batch, range.mVertices.mOffset, range.mVertices.in(scene.meshes().getTexCoords()));
            mColours.writeAt(batch, range.mVertices.mOffset, range.mVertices.in(scene.meshes().getColours()));

            if (!range.mSecondTexCoords.empty())
                mSecondTexCoords.writeAt(batch, range.mSecondTexCoords.mOffset,
                    range.mSecondTexCoords.in(scene.meshes().getSecondTexCoords()));
        }

        // Whole, and it is twenty-four bytes a slot. A mesh arriving moves nothing already in this,
        // but sizing it to the scene means growing it, and growing means writing it — so the rows
        // that did not change are written again for the price of not having to know which did.
        mMeshScratch.clear();
        mMeshScratch.reserve(scene.meshes().getRows().size());
        for (const MeshRange& mesh : scene.meshes().getRows())
            mMeshScratch.push_back(Shaders::GpuMesh{
                .mVertexOffset = mesh.mVertices.mOffset,
                .mIndexOffset = mesh.mIndices.mOffset,
                .mShape
                = (mesh.mShape.mSheet ? Shaders::MESH_SHEET : 0u) | (mesh.mShape.mClosed ? Shaders::MESH_CLOSED : 0u),
                .mSecondTexCoordOffset
                = mesh.mSecondTexCoords.empty() ? Shaders::NO_STREAM : mesh.mSecondTexCoords.mOffset,
                .mUnitStreams = mesh.mUnitStreams,
                .mBindOffset = mesh.mDeform != Deform::None ? mesh.mBindOffset : Shaders::NO_STREAM,
            });

        // **A new table on every arrival, and the old one buried**, because the frame behind is
        // reading the old one: an arrival does not wait for the frames in flight, and a slot the
        // scene handed out again holds another mesh's offsets in the same row — which the frame
        // behind, still tracing the mesh that was there, would read as that mesh's geometry. One
        // copy is the versioned kind, and a version is never written in place.
        const VkDeviceSize bytes = mMeshScratch.size() * sizeof(Shaders::GpuMesh);
        mDevice.getGraveyard().replace(mMeshes, Buffer::hostWritten(mDevice, bytes, sTableUsage, "meshes"));
        mMeshes.write(std::span<const Shaders::GpuMesh>(mMeshScratch));
    }

    SpriteSource SceneBuffers::describeSprites(const FrameSlot slot) const
    {
        // The bin copies the one and reads the other in the submit it records into, which is
        // the next one: the address says so of the emitters, and the copy of the sprites.
        const Tables& tables = mTables.at(slot);

        return SpriteSource{
            .mSprites = &tables.mSprites,
            .mEmitters = tables.mEmitters.addressFor(),
            .mSpriteCount = tables.mSpriteCount,
            .mEmitterCount = tables.mEmitterCount,
        };
    }

    void SceneBuffers::shade(const SceneDesc& scene, const FrameSlot slot)
    {
        const std::span<const Material> materials = scene.materials().getRows();

        // Every row where the table changed length, and the rows the scene wrote otherwise. The
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
            for (const Index at : scene.materials().getWritten())
                mMaterialTable.write(at) = toGpu(materials[at]);
        }

        mMaterialTable.sync(slot);

        assert(mStagedRuns == scene.materials().getRunRevision()
            && "layer or mask runs arrived without an extend to stage them");
    }

    void SceneBuffers::writeMaterialRuns(Batch& batch, const SceneDesc& scene)
    {
        const std::span<const MaterialLayer> layers = scene.materials().getLayers();
        const std::span<const float> masks = scene.materials().getMasks();

        // A scene with no terrain in it still has to bind something: a descriptor may not be null,
        // and a zero-length buffer is not a thing Vulkan will make. One unread element each — and
        // the layer cannot be `constexpr`, because `osg::Vec4f` has no constexpr default.
        const Shaders::GpuLayer noLayer{};
        constexpr float noMask = 1.0f;

        // On the queue, whole where the table was made again and run by run otherwise: a run an
        // arrival was given may be one a frame in flight still reads of the material that held it
        // last, and the copy recorded here runs behind that frame. What a flipbook does every frame
        // never touches these tables.
        if (outgrow(mLayers, mDevice, BufferKind::DeviceLocal,
                std::max<std::size_t>(layers.size(), 1) * sizeof(Shaders::GpuLayer), sTableFilledUsage, "layers"))
            stageInto(batch, mLayers, 0,
                std::as_bytes(layers.empty() ? std::span<const Shaders::GpuLayer>(&noLayer, 1) : layers));
        else
            // Each run as the chunk placed it, staged at the run's own offset, so a table of a
            // thousand layers pays for the five that arrived.
            for (const Run run : scene.materials().getArrived().mLayers)
                stageInto(batch, mLayers, run.mOffset * sizeof(Shaders::GpuLayer), std::as_bytes(run.in(layers)));

        if (outgrow(mMasks, mDevice, BufferKind::DeviceLocal, std::max<std::size_t>(masks.size(), 1) * sizeof(float),
                sTableFilledUsage, "masks"))
            stageInto(batch, mMasks, 0, std::as_bytes(masks.empty() ? std::span<const float>(&noMask, 1) : masks));
        else
            for (const Run run : scene.materials().getArrived().mMasks)
                stageInto(batch, mMasks, run.mOffset * sizeof(float), std::as_bytes(run.in(masks)));

        mStagedRuns = scene.materials().getRunRevision();
    }

    void SceneBuffers::place(const SceneDesc& scene, std::span<const InstanceRecord> records,
        std::span<const Index> changed, const Placing& placing)
    {
        const FrameSlot slot = placing.mSlot;
        shade(scene, slot);

        Tables& tables = mTables.at(slot);

        // The sentinel material sits one past the real ones, which is where `shade` put it.
        const auto sentinel = static_cast<std::uint32_t>(scene.materials().getRows().size());

        // Indexed by slot, gaps included. A hit reads its slot back as the custom index and
        // looks the row up here directly, so a table that closed its gaps would answer for the
        // wrong placement. A gap's row is never read, so it is never written either.
        const std::span<const PlacementRow> placements = scene.placements().getRows();

        const std::size_t had = mInstanceTable.size();
        mInstanceTable.resize(records.size());

        const auto placeRow = [&](const std::size_t at) {
            const InstanceRecord& record = records[at];
            if (!record.mPlaced)
                return;

            Shaders::GpuInstance& row = mInstanceTable.write(static_cast<Index>(at));
            row.mMesh = record.mMesh;
            const MeshInstance& placed = placements[at].mInstance;
            row.mMaterial = placed.mMaterial == sNoIndex ? sentinel : placed.mMaterial;
            row.mOpacity = placed.mOpacity;

            for (int r = 0; r < 3; ++r)
                row.mMotion[r] = osg::Vec4f(record.mMotion.mRows[r][0], record.mMotion.mRows[r][1],
                    record.mMotion.mRows[r][2], record.mMotion.mRows[r][3]);
        };

        // The rows this placement wrote, and whatever the table grew by. A world is tens of
        // thousands of placements and a frame moves hundreds; writing every row to change those was
        // a memcpy of megabytes a frame. Which copies are then behind is the table's own answer,
        // and it is the same answer the acceleration structure's rows get from the same list.
        for (std::size_t at = had; at < records.size(); ++at)
            placeRow(at);

        for (const Index at : changed)
            placeRow(at);

        mInstanceTable.sync(slot);

        mLightGrid.rebuild(scene.lights());

        // The tables go over as they lie: the scene's rows are the device's, so a placement is a
        // copy and never a conversion. Empty ones included: something has to stand at every
        // address the frame carries, and `growTo` makes a table that is empty rather than leaving
        // the slot empty — a stand-in per table is one table without one, and that costs a
        // device. What stops the shader reading an empty table is its count.
        const std::span<const Light> lights = scene.lights();
        const std::span<const std::uint32_t> lightList = mLightGrid.getList().getWhole();
        const std::span<const SpriteEmitter> emitters = scene.emitters();
        const std::span<const Sprite> sprites = scene.sprites();

        growTo(tables.mLights, mDevice, BufferKind::HostWritten, lights.size_bytes(), sTableUsage, "lights");
        growTo(tables.mLightList, mDevice, BufferKind::HostWritten, lightList.size_bytes(), sTableUsage, "light list");
        growTo(tables.mEmitters, mDevice, BufferKind::HostWritten, emitters.size_bytes(), sTableUsage, "emitters");
        growTo(
            tables.mSprites, mDevice, BufferKind::HostWritten, sprites.size_bytes(), sTableCopiedFromUsage, "sprites");

        tables.mLights.write(lights);
        tables.mLightList.write(lightList);
        tables.mEmitters.write(emitters);

        // Unshaded, which is what a trace's bin copies and shades for its own sun.
        tables.mSprites.write(sprites);
        tables.mSpriteCount = static_cast<std::uint32_t>(sprites.size());
        tables.mEmitterCount = static_cast<std::uint32_t>(emitters.size());

        // The normals of anything skinned are not written here: a cell's are the same from one
        // frame to the next, and a body's are what `SkinPass` computed into this copy ahead of this.
    }

    void SceneBuffers::finishReads(const FrameSlot slot) const
    {
        mInstanceTable.finishReads(slot);
        mMaterialTable.finishReads(slot);

        const Tables& tables = mTables.at(slot);
        tables.mLights.waitIdle("a trace still reading a copy's lights");
        tables.mLightList.waitIdle("a trace still reading a copy's light list");
        tables.mEmitters.waitIdle("a trace still reading a copy's emitters");
        tables.mSprites.waitIdle("a trace's bin still copying a copy's sprites");
    }

    void SceneBuffers::describeTables(const FrameSlot slot, Shaders::GpuTables& into) const
    {
        const Tables& tables = mTables.at(slot);

        // What the block hands out, the submit it is recorded into reads — which is the next one,
        // whether it is a frame's trace or a picture's deferred batch — and `addressFor` is what
        // says so of each. Every host write of any of these then checks the stamp, so a table
        // rewritten under a trace is an assert and not a fault.
        into.mNormalBlocks = mNormalTable.at(slot).getTableAddress();
        into.mTexCoordBlocks = mTexCoords.getTableAddress();
        into.mColourBlocks = mColours.getTableAddress();
        into.mSecondTexCoordBlocks = mSecondTexCoords.getTableAddress();
        into.mMeshes = mMeshes.addressFor();
        into.mInstances = mInstanceTable.addressFor(slot);
        into.mMaterials = mMaterialTable.addressFor(slot);
        into.mLayers = mLayers.addressFor();
        into.mMasks = mMasks.addressFor();
        into.mLights = tables.mLights.addressFor();
        into.mLightList = tables.mLightList.addressFor();
        into.mEmitters = tables.mEmitters.addressFor();
    }

    VkDeviceSize SceneBuffers::Tables::getBytes() const
    {
        return mLights.getSize() + mLightList.getSize() + mSprites.getSize() + mEmitters.getSize();
    }

    VkDeviceSize SceneBuffers::getBytes() const
    {
        // The indices are not counted here: they belong to the acceleration structure, which reports
        // its own size.
        VkDeviceSize total = mTexCoords.getBytes() + mSecondTexCoords.getBytes() + mColours.getBytes()
            + mMeshes.getSize() + mLayers.getSize() + mMasks.getSize() + mInstanceTable.getBytes()
            + mMaterialTable.getBytes() + mNormalTable.getBytes() + mTangentTable.getBytes();
        for (const Tables& tables : mTables.live())
            total += tables.getBytes();

        return total;
    }
}
