#include "skintables.hpp"

#include <cassert>
#include <cstddef>

#include <osg/Vec3f>

#include <components/rtx/deformertable.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/shaders/skinning.h>

#include "bufferusage.hpp"
#include "commands.hpp"
#include "device.hpp"

namespace Rtx
{
    namespace
    {
        /// Every index below `count`, refilled into `into`.
        std::span<const Index> everyBelow(std::size_t count, std::vector<Index>& into)
        {
            into.resize(count);
            for (std::size_t at = 0; at < count; ++at)
                into[at] = static_cast<Index>(at);

            return into;
        }
    }

    SkinTables::SkinTables(const Device& device, Batch& batch, const SceneDesc& scene, const std::uint32_t slots)
        : mDevice(device)
    {
        mBones.open(slots);
        mWeights.open(slots);

        // Every table exists from here, whether or not anything has been written to it: `outgrow`
        // makes a table that is empty whatever it is asked for, so a scene with no actor in it
        // still has a buffer at every address a dispatch could be handed.
        extend(batch, scene);
    }

    void SkinTables::extend(Batch& batch, const SceneDesc& scene)
    {
        const Device& device = mDevice;

        // Grown to what the scene reaches, and written whole where a growth moved it. The
        // arrivals are what a frame with an actor walking in costs; a table made again is what a
        // cell full of them costs, once per doubling.
        const VkDeviceSize bind = VkDeviceSize{ scene.deformers().getBindVertexCount() } * sizeof(osg::Vec3f);
        const bool bindMoved
            = outgrow(mBindPositions, device, BufferKind::DeviceLocal, bind, sTableFilledUsage, "bind positions")
            | outgrow(mBindNormals, device, BufferKind::DeviceLocal, bind, sTableFilledUsage, "bind normals");
        writeBind(batch, scene, scene.meshes().getArrived(), bindMoved);

        const bool rigsMoved
            = outgrow(mRuns, device, BufferKind::DeviceLocal,
                  scene.deformers().getRuns().size() * sizeof(std::uint32_t), sTableFilledUsage, "rig runs")
            | outgrow(mInfluences, device, BufferKind::DeviceLocal,
                scene.deformers().getInfluences().size() * sizeof(Shaders::GpuInfluence), sTableFilledUsage,
                "rig influences");
        writeRigs(batch, scene, scene.deformers().getArrivedRigs(), rigsMoved);

        const bool morphsMoved = outgrow(mMorphOffsets, device, BufferKind::DeviceLocal,
            scene.deformers().getMorphOffsets().size() * sizeof(osg::Vec3f), sTableFilledUsage, "morph offsets");
        writeMorphs(batch, scene, scene.deformers().getArrivedMorphs(), morphsMoved);

        // The arrivals' rows into the first copy alone. Every other row of a copy reaches it in
        // the placement that dispatches over it, and the other copies owe the arrivals theirs.
        for (Buffer& bones : mBones.live())
            outgrow(bones, device, BufferKind::HostWritten,
                scene.deformers().getBones().size() * sizeof(Shaders::GpuBone), sTableFilledUsage, "bones");
        for (Buffer& weights : mWeights.live())
            outgrow(weights, device, BufferKind::HostWritten, scene.deformers().getWeights().size() * sizeof(float),
                sTableFilledUsage, "weights");
        writeRows(batch, scene, scene.meshes().getArrived());

        orderStagedWrites(batch);
    }

    void SkinTables::writeBind(
        Batch& batch, const SceneDesc& scene, const std::span<const Index> meshes, const bool whole)
    {
        const std::span<const MeshRange> ranges = scene.meshes().getRows();
        for (const Index index : whole ? everyBelow(ranges.size(), mEvery) : meshes)
        {
            const MeshRange& mesh = ranges[index];
            if (mesh.mDeform == Deform::None || mesh.mVertices.mCount == 0)
                continue;

            const VkDeviceSize at = VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
            stageInto(batch, mBindPositions, at, std::as_bytes(scene.meshes().getMeshPositions(index)));
            stageInto(batch, mBindNormals, at, std::as_bytes(mesh.mVertices.in(scene.meshes().getNormals())));
        }
    }

    void SkinTables::writeRigs(
        Batch& batch, const SceneDesc& scene, const std::span<const Index> rigs, const bool whole)
    {
        const std::span<const Rig> table = scene.deformers().getRigs();
        for (const Index index : whole ? everyBelow(table.size(), mEvery) : rigs)
        {
            // A freed slot skins nothing and holds no run to write.
            const Rig& rig = table[index];
            if (rig.mRuns.empty())
                continue;

            stageInto(batch, mRuns, VkDeviceSize{ rig.mRuns.mOffset } * sizeof(std::uint32_t),
                std::as_bytes(rig.mRuns.in(scene.deformers().getRuns())));
            stageInto(batch, mInfluences, VkDeviceSize{ rig.mInfluences.mOffset } * sizeof(Shaders::GpuInfluence),
                std::as_bytes(rig.mInfluences.in(scene.deformers().getInfluences())));
        }
    }

    void SkinTables::writeMorphs(
        Batch& batch, const SceneDesc& scene, const std::span<const Index> morphs, const bool whole)
    {
        const std::span<const Morph> table = scene.deformers().getMorphs();
        for (const Index index : whole ? everyBelow(table.size(), mEvery) : morphs)
        {
            const Morph& morph = table[index];
            if (morph.mOffsets.empty())
                continue;

            stageInto(batch, mMorphOffsets, VkDeviceSize{ morph.mOffsets.mOffset } * sizeof(osg::Vec3f),
                std::as_bytes(morph.mOffsets.in(scene.deformers().getMorphOffsets())));
        }
    }

    void SkinTables::writeRows(Batch& batch, const SceneDesc& scene, const std::span<const Index> meshes)
    {
        const std::span<const MeshRange> ranges = scene.meshes().getRows();
        for (const Index index : meshes)
        {
            const MeshRange& mesh = ranges[index];
            if (mesh.mDeform == Deform::Rig)
                stageInto(batch, mBones.at(FrameSlot{}), VkDeviceSize{ mesh.mPoseOffset } * sizeof(Shaders::GpuBone),
                    std::as_bytes(scene.getMeshBones(index)));
            else if (mesh.mDeform == Deform::Morph)
                stageInto(batch, mWeights.at(FrameSlot{}), VkDeviceSize{ mesh.mPoseOffset } * sizeof(float),
                    std::as_bytes(scene.getMeshWeights(index)));
        }
    }

    void SkinTables::finishReads(const FrameSlot slot) const
    {
        mBones.at(slot).waitIdle("an arrival's pose over the rows a placement writes");
        mWeights.at(slot).waitIdle("an arrival's morph over the weights a placement writes");
    }

    VkDeviceAddress SkinTables::writeBones(const SceneDesc& scene, const FrameSlot slot, const Index mesh)
    {
        const MeshRange& range = scene.meshes().getRows()[mesh];
        mBones.at(slot).writeAt(VkDeviceSize{ range.mPoseOffset } * sizeof(Shaders::GpuBone), scene.getMeshBones(mesh));

        return getBones(range, slot);
    }

    VkDeviceAddress SkinTables::writeWeights(const SceneDesc& scene, const FrameSlot slot, const Index mesh)
    {
        const MeshRange& range = scene.meshes().getRows()[mesh];
        mWeights.at(slot).writeAt(VkDeviceSize{ range.mPoseOffset } * sizeof(float), scene.getMeshWeights(mesh));

        return getWeights(range, slot);
    }

    VkDeviceAddress SkinTables::getBones(const MeshRange& mesh, const FrameSlot slot) const
    {
        const VkDeviceAddress address
            = mBones.at(slot).addressFor() + VkDeviceSize{ mesh.mPoseOffset } * sizeof(Shaders::GpuBone);
        assert(
            address % Shaders::BONE_ALIGN == 0 && "a run of rows the kernel's reference claims more of than is true");
        return address;
    }

    VkDeviceAddress SkinTables::getWeights(const MeshRange& mesh, const FrameSlot slot) const
    {
        return mWeights.at(slot).addressFor() + VkDeviceSize{ mesh.mPoseOffset } * sizeof(float);
    }

    VkDeviceAddress SkinTables::getBindPositions(const MeshRange& mesh) const
    {
        return mBindPositions.addressFor() + VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceAddress SkinTables::getBindNormals(const MeshRange& mesh) const
    {
        return mBindNormals.addressFor() + VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceAddress SkinTables::getRuns(const Rig& rig) const
    {
        return mRuns.addressFor() + VkDeviceSize{ rig.mRuns.mOffset } * sizeof(std::uint32_t);
    }

    VkDeviceAddress SkinTables::getInfluences(const Rig& rig) const
    {
        return mInfluences.addressFor() + VkDeviceSize{ rig.mInfluences.mOffset } * sizeof(Shaders::GpuInfluence);
    }

    VkDeviceAddress SkinTables::getMorphOffsets(const Morph& morph) const
    {
        return mMorphOffsets.addressFor() + VkDeviceSize{ morph.mOffsets.mOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceSize SkinTables::getBytes() const
    {
        VkDeviceSize total = mBindPositions.getSize() + mBindNormals.getSize() + mRuns.getSize() + mInfluences.getSize()
            + mMorphOffsets.getSize();
        for (const Buffer& bones : mBones.live())
            total += bones.getSize();
        for (const Buffer& weights : mWeights.live())
            total += weights.getSize();

        return total;
    }
}
