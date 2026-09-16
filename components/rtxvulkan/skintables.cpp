#include "skintables.hpp"

#include <algorithm>
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
        mPoses.open(slots);

        // Every table exists from here, whether or not anything has been written to it: `outgrow`
        // makes a table that is empty whatever it is asked for, so a scene with no actor in it
        // still has a buffer at every address a dispatch could be handed.
        extend(batch, scene);
    }

    void SkinTables::extend(Batch& batch, const SceneDesc& scene)
    {
        const Device& device = mDevice;
        const DeformerTable& deformers = scene.deformers();

        // Grown to what the scene reaches, and written whole where a growth moved it. The
        // arrivals are what a frame with an actor walking in costs; a table made again is what a
        // cell full of them costs, once per doubling.
        const VkDeviceSize bind = VkDeviceSize{ deformers.getBindVertexCount() } * sizeof(osg::Vec3f);
        const bool bindMoved
            = outgrow(mBindPositions, device, BufferKind::DeviceLocal, bind, sTableFilledUsage, "bind positions")
            | outgrow(mBindNormals, device, BufferKind::DeviceLocal, bind, sTableFilledUsage, "bind normals");
        writeBind(batch, scene, scene.meshes().getArrived(), bindMoved);

        const Moved moved{
            .mRuns = outgrow(mRuns, device, BufferKind::DeviceLocal, deformers.getRuns().size() * sizeof(std::uint32_t),
                sTableFilledUsage, "rig runs"),
            .mInfluences = outgrow(mInfluences, device, BufferKind::DeviceLocal,
                deformers.getInfluences().size() * sizeof(Shaders::GpuInfluence), sTableFilledUsage, "rig influences"),
            .mOffsets = outgrow(mMorphOffsets, device, BufferKind::DeviceLocal,
                deformers.getMorphOffsets().size() * sizeof(osg::Vec3f), sTableFilledUsage, "morph offsets"),
        };
        writeDeformers(batch, scene, deformers.getArrived(), moved);

        // The arrivals' poses into the first copy alone. Every other pose of a copy reaches it in
        // the placement that dispatches over it, and the other copies owe the arrivals theirs.
        for (Buffer& poses : mPoses.live())
            outgrow(poses, device, BufferKind::HostWritten, deformers.getPoses().size() * sizeof(PoseWord),
                sTableFilledUsage, "poses");
        writePoses(batch, scene, scene.meshes().getArrived());

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

    void SkinTables::writeDeformers(
        Batch& batch, const SceneDesc& scene, const std::span<const Index> arrived, const Moved moved)
    {
        const DeformerTable& deformers = scene.deformers();
        const std::span<const Deformer> table = deformers.getDeformers();
        const bool whole = moved.mRuns || moved.mInfluences || moved.mOffsets;

        for (const Index index : whole ? everyBelow(table.size(), mEvery) : arrived)
        {
            // A freed slot deforms nothing and holds no run to write. An arrival stages every run
            // it holds; a row walked for a table that moved stages the runs into that table, and
            // the other tables keep what they hold.
            const Deformer& deformer = table[index];
            const bool fresh = !whole || std::find(arrived.begin(), arrived.end(), index) != arrived.end();

            if (!deformer.mRuns.empty() && (fresh || moved.mRuns))
                stageInto(batch, mRuns, VkDeviceSize{ deformer.mRuns.mOffset } * sizeof(std::uint32_t),
                    std::as_bytes(deformer.mRuns.in(deformers.getRuns())));

            if (!deformer.mInfluences.empty() && (fresh || moved.mInfluences))
                stageInto(batch, mInfluences,
                    VkDeviceSize{ deformer.mInfluences.mOffset } * sizeof(Shaders::GpuInfluence),
                    std::as_bytes(deformer.mInfluences.in(deformers.getInfluences())));

            if (!deformer.mOffsets.empty() && (fresh || moved.mOffsets))
                stageInto(batch, mMorphOffsets, VkDeviceSize{ deformer.mOffsets.mOffset } * sizeof(osg::Vec3f),
                    std::as_bytes(deformer.mOffsets.in(deformers.getMorphOffsets())));
        }
    }

    void SkinTables::writePoses(Batch& batch, const SceneDesc& scene, const std::span<const Index> meshes)
    {
        const std::span<const MeshRange> ranges = scene.meshes().getRows();
        for (const Index index : meshes)
        {
            const MeshRange& mesh = ranges[index];
            if (mesh.mDeform == Deform::None)
                continue;

            stageInto(batch, mPoses.at(FrameSlot{}), VkDeviceSize{ mesh.mPoseOffset } * sizeof(PoseWord),
                std::as_bytes(scene.getMeshPose(index)));
        }
    }

    void SkinTables::finishReads(const FrameSlot slot) const
    {
        mPoses.at(slot).waitIdle("an arrival's pose over the poses a placement writes");
    }

    VkDeviceAddress SkinTables::writePose(const SceneDesc& scene, const FrameSlot slot, const Index mesh)
    {
        const MeshRange& range = scene.meshes().getRows()[mesh];
        mPoses.at(slot).writeAt(VkDeviceSize{ range.mPoseOffset } * sizeof(PoseWord), scene.getMeshPose(mesh));

        return getPose(range, slot);
    }

    VkDeviceAddress SkinTables::getPose(const MeshRange& mesh, const FrameSlot slot) const
    {
        const VkDeviceAddress address
            = mPoses.at(slot).addressFor() + VkDeviceSize{ mesh.mPoseOffset } * sizeof(PoseWord);
        assert(
            address % Shaders::BONE_ALIGN == 0 && "a run of rows the kernel's reference claims more of than is true");
        return address;
    }

    VkDeviceAddress SkinTables::getBindPositions(const MeshRange& mesh) const
    {
        return mBindPositions.addressFor() + VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceAddress SkinTables::getBindNormals(const MeshRange& mesh) const
    {
        return mBindNormals.addressFor() + VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceAddress SkinTables::getRuns(const Deformer& rig) const
    {
        return mRuns.addressFor() + VkDeviceSize{ rig.mRuns.mOffset } * sizeof(std::uint32_t);
    }

    VkDeviceAddress SkinTables::getInfluences(const Deformer& rig) const
    {
        return mInfluences.addressFor() + VkDeviceSize{ rig.mInfluences.mOffset } * sizeof(Shaders::GpuInfluence);
    }

    VkDeviceAddress SkinTables::getMorphOffsets(const Deformer& morph) const
    {
        return mMorphOffsets.addressFor() + VkDeviceSize{ morph.mOffsets.mOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceSize SkinTables::getBytes() const
    {
        VkDeviceSize total = mBindPositions.getSize() + mBindNormals.getSize() + mRuns.getSize() + mInfluences.getSize()
            + mMorphOffsets.getSize();
        for (const Buffer& poses : mPoses.live())
            total += poses.getSize();

        return total;
    }
}
