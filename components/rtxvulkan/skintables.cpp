#include "skintables.hpp"

#include <cassert>
#include <cstddef>

#include "bufferusage.hpp"
#include "device.hpp"
#include "graveyard.hpp"

namespace Rtx
{
    namespace
    {
        /// What a reference to a row of `GpuBone`s claims of every address it is constructed from.
        /// A claim larger than the truth is undefined behaviour with no message, so the host checks
        /// it where the address is made.
        constexpr VkDeviceAddress sBoneAlignment = 16;

        /// Every index below `count`, refilled into `into`.
        std::span<const Index> everyBelow(std::size_t count, std::vector<Index>& into)
        {
            into.resize(count);
            for (std::size_t at = 0; at < count; ++at)
                into[at] = static_cast<Index>(at);

            return into;
        }
    }

    SkinTables::SkinTables(
        const Device& device, const SceneDesc& scene, const std::uint32_t slots, Graveyard& graveyard)
        : mDevice(&device)
    {
        mBones.open(slots);
        mWeights.open(slots);

        // Every table exists from here, whether or not anything has been written to it: `outgrow`
        // makes a table that is empty whatever it is asked for, so a scene with no actor in it
        // still has a buffer at every address a dispatch could be handed.
        extend(scene, graveyard);
    }

    void SkinTables::extend(const SceneDesc& scene, Graveyard& graveyard)
    {
        const Device& device = *mDevice;

        // Grown to what the scene reaches, and written whole where a growth moved it. The
        // arrivals are what a frame with an actor walking in costs; a table made again is what a
        // cell full of them costs, once per doubling.
        const VkDeviceSize bind = VkDeviceSize{ scene.deformers().getBindVertexCount() } * sizeof(osg::Vec3f);
        const bool bindMoved
            = outgrow(mBindPositions, device, BufferKind::HostWritten, bind, sTableUsage, "bind positions", graveyard)
            | outgrow(mBindNormals, device, BufferKind::HostWritten, bind, sTableUsage, "bind normals", graveyard);
        writeBind(scene, scene.meshes().getArrived(), bindMoved);

        const bool rigsMoved
            = outgrow(mRuns, device, BufferKind::HostWritten,
                  scene.deformers().getRuns().size() * sizeof(std::uint32_t), sTableUsage, "rig runs", graveyard)
            | outgrow(mInfluences, device, BufferKind::HostWritten,
                scene.deformers().getInfluences().size() * sizeof(Shaders::GpuInfluence), sTableUsage, "rig influences",
                graveyard);
        writeRigs(scene, scene.deformers().getArrivedRigs(), rigsMoved);

        const bool morphsMoved = outgrow(mMorphOffsets, device, BufferKind::HostWritten,
            scene.deformers().getMorphOffsets().size() * sizeof(osg::Vec3f), sTableUsage, "morph offsets", graveyard);
        writeMorphs(scene, scene.deformers().getArrivedMorphs(), morphsMoved);

        // Nothing is written into these on arrival: a mesh's rows reach a copy in the placement
        // that dispatches over them, and not before.
        for (Buffer& bones : mBones.live())
            outgrow(bones, device, BufferKind::HostWritten,
                scene.deformers().getBones().size() * sizeof(Shaders::GpuBone), sTableUsage, "bones", graveyard);
        for (Buffer& weights : mWeights.live())
            outgrow(weights, device, BufferKind::HostWritten, scene.deformers().getWeights().size() * sizeof(float),
                sTableUsage, "weights", graveyard);
    }

    void SkinTables::writeBind(const SceneDesc& scene, std::span<const Index> meshes, const bool whole)
    {
        const std::span<const MeshRange> ranges = scene.meshes().getRows();
        for (const Index index : whole ? everyBelow(ranges.size(), mEvery) : meshes)
        {
            const MeshRange& mesh = ranges[index];
            if (mesh.mDeform == Deform::None || mesh.mVertices.mCount == 0)
                continue;

            // Appended and not rewritten: an arrival's run is one no frame in flight reads, and a
            // table made again is read by nothing yet.
            const VkDeviceSize at = VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
            mBindPositions.appendAt(at, scene.meshes().getMeshPositions(index));
            mBindNormals.appendAt(at, mesh.mVertices.in(scene.meshes().getNormals()));
        }
    }

    void SkinTables::writeRigs(const SceneDesc& scene, std::span<const Index> rigs, const bool whole)
    {
        const std::span<const Rig> table = scene.deformers().getRigs();
        for (const Index index : whole ? everyBelow(table.size(), mEvery) : rigs)
        {
            // A freed slot skins nothing and holds no run to write.
            const Rig& rig = table[index];
            if (rig.mRuns.empty())
                continue;

            mRuns.appendAt(
                VkDeviceSize{ rig.mRuns.mOffset } * sizeof(std::uint32_t), rig.mRuns.in(scene.deformers().getRuns()));
            mInfluences.appendAt(VkDeviceSize{ rig.mInfluences.mOffset } * sizeof(Shaders::GpuInfluence),
                rig.mInfluences.in(scene.deformers().getInfluences()));
        }
    }

    void SkinTables::writeMorphs(const SceneDesc& scene, std::span<const Index> morphs, const bool whole)
    {
        const std::span<const Morph> table = scene.deformers().getMorphs();
        for (const Index index : whole ? everyBelow(table.size(), mEvery) : morphs)
        {
            const Morph& morph = table[index];
            if (morph.mOffsets.empty())
                continue;

            mMorphOffsets.appendAt(VkDeviceSize{ morph.mOffsets.mOffset } * sizeof(osg::Vec3f),
                morph.mOffsets.in(scene.deformers().getMorphOffsets()));
        }
    }

    VkDeviceAddress SkinTables::writeBones(
        const SceneDesc& scene, const FrameSlot slot, const Index mesh, const Rows rows)
    {
        const MeshRange& range = scene.meshes().getRows()[mesh];
        const VkDeviceSize at = VkDeviceSize{ range.mPoseOffset } * sizeof(Shaders::GpuBone);
        const Buffer& into = mBones.at(slot);
        if (rows == Rows::Arrived)
            into.appendAt(at, scene.getMeshBones(mesh));
        else
            into.writeAt(at, scene.getMeshBones(mesh));

        const VkDeviceAddress address = into.addressFor() + at;
        assert(address % sBoneAlignment == 0 && "a run of rows the kernel's reference claims more of than is true");
        return address;
    }

    VkDeviceAddress SkinTables::writeWeights(
        const SceneDesc& scene, const FrameSlot slot, const Index mesh, const Rows rows)
    {
        const MeshRange& range = scene.meshes().getRows()[mesh];
        const VkDeviceSize at = VkDeviceSize{ range.mPoseOffset } * sizeof(float);
        const Buffer& into = mWeights.at(slot);
        if (rows == Rows::Arrived)
            into.appendAt(at, scene.getMeshWeights(mesh));
        else
            into.writeAt(at, scene.getMeshWeights(mesh));

        return into.addressFor() + at;
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
