#include "skintables.hpp"

#include <cassert>
#include <cstddef>

#include "device.hpp"
#include "graveyard.hpp"

namespace Rtx
{
    namespace
    {
        // Addressable and never bound: a dispatch is handed every run's address in its push
        // constants, and no descriptor names one of these.
        constexpr VkBufferUsageFlags sTableUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

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
        const Device& device, const SceneTables& scene, const std::uint32_t slots, Graveyard& graveyard)
        : mDevice(&device)
        , mSlots(slots)
    {
        assert(slots >= 1 && slots <= sFrameSlots && "more frames in flight than there are copies of the rows");

        // **Every table exists from here, whether or not anything has been written to it.** A
        // scene with no actor in it poses nothing and reads none of these, and a scene that gains
        // one grows them; what `growTo` guarantees is that there is a buffer to grow.
        for (Buffer* table : { &mBindPositions, &mBindNormals, &mRuns, &mInfluences, &mMorphOffsets })
            graveyard.bury(growTo(*table, device, 0, sTableUsage));

        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
        {
            graveyard.bury(growTo(mBones[slot], device, 0, sTableUsage));
            graveyard.bury(growTo(mWeights[slot], device, 0, sTableUsage));
        }

        extend(scene, graveyard);
    }

    void SkinTables::extend(const SceneTables& scene, Graveyard& graveyard)
    {
        const Device& device = *mDevice;

        // **Grown to what the scene reaches, and written whole where a growth moved it.** The
        // arrivals are what a frame with an actor walking in costs; a table made again is what a
        // cell full of them costs, once per doubling.
        const VkDeviceSize bind = VkDeviceSize{ scene.mDeformers.getBindVertexCount() } * sizeof(osg::Vec3f);
        const bool bindMoved = outgrow(mBindPositions, device, bind, sTableUsage, graveyard)
            | outgrow(mBindNormals, device, bind, sTableUsage, graveyard);
        writeBind(scene, scene.mMeshes.getArrived(), bindMoved);

        const bool rigsMoved
            = outgrow(mRuns, device, scene.mDeformers.getRuns().size() * sizeof(std::uint32_t), sTableUsage, graveyard)
            | outgrow(mInfluences, device, scene.mDeformers.getInfluences().size() * sizeof(Shaders::GpuInfluence),
                sTableUsage, graveyard);
        writeRigs(scene, scene.mDeformers.getArrivedRigs(), rigsMoved);

        const bool morphsMoved = outgrow(mMorphOffsets, device,
            scene.mDeformers.getMorphOffsets().size() * sizeof(osg::Vec3f), sTableUsage, graveyard);
        writeMorphs(scene, scene.mDeformers.getArrivedMorphs(), morphsMoved);

        // Nothing is written into these on arrival: a mesh's rows reach a copy in the placement
        // that dispatches over them, and not before.
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
        {
            outgrow(mBones[slot], device, scene.mDeformers.getBones().size() * sizeof(Shaders::GpuBone), sTableUsage,
                graveyard);
            outgrow(
                mWeights[slot], device, scene.mDeformers.getWeights().size() * sizeof(float), sTableUsage, graveyard);
        }
    }

    void SkinTables::writeBind(const SceneTables& scene, std::span<const Index> meshes, const bool whole)
    {
        const std::span<const MeshRange> ranges = scene.mMeshes.getRows();
        for (const Index index : whole ? everyBelow(ranges.size(), mEvery) : meshes)
        {
            const MeshRange& mesh = ranges[index];
            if (mesh.mDeform == Deform::None || mesh.mVertices.mCount == 0)
                continue;

            const VkDeviceSize at = VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
            mBindPositions.writeAt(at, scene.mMeshes.getMeshPositions(index));
            mBindNormals.writeAt(at, mesh.mVertices.in(scene.mMeshes.getNormals()));
        }
    }

    void SkinTables::writeRigs(const SceneTables& scene, std::span<const Index> rigs, const bool whole)
    {
        const std::span<const Rig> table = scene.mDeformers.getRigs();
        for (const Index index : whole ? everyBelow(table.size(), mEvery) : rigs)
        {
            // A freed slot skins nothing and holds no run to write.
            const Rig& rig = table[index];
            if (rig.mRuns.empty())
                continue;

            mRuns.writeAt(
                VkDeviceSize{ rig.mRuns.mOffset } * sizeof(std::uint32_t), rig.mRuns.in(scene.mDeformers.getRuns()));
            mInfluences.writeAt(VkDeviceSize{ rig.mInfluences.mOffset } * sizeof(Shaders::GpuInfluence),
                rig.mInfluences.in(scene.mDeformers.getInfluences()));
        }
    }

    void SkinTables::writeMorphs(const SceneTables& scene, std::span<const Index> morphs, const bool whole)
    {
        const std::span<const Morph> table = scene.mDeformers.getMorphs();
        for (const Index index : whole ? everyBelow(table.size(), mEvery) : morphs)
        {
            const Morph& morph = table[index];
            if (morph.mOffsets.empty())
                continue;

            mMorphOffsets.writeAt(VkDeviceSize{ morph.mOffsets.mOffset } * sizeof(osg::Vec3f),
                morph.mOffsets.in(scene.mDeformers.getMorphOffsets()));
        }
    }

    VkDeviceAddress SkinTables::writeBones(const SceneTables& scene, const FrameSlot slot, const Index mesh)
    {
        assert(slot.get() < mSlots);

        const MeshRange& range = scene.mMeshes.getRows()[mesh];
        const VkDeviceSize at = VkDeviceSize{ range.mPoseOffset } * sizeof(Shaders::GpuBone);
        mBones[slot.get()].writeAt(at, scene.getMeshBones(mesh));

        const VkDeviceAddress address = mBones[slot.get()].getDeviceAddress() + at;
        assert(address % sBoneAlignment == 0 && "a run of rows the kernel's reference claims more of than is true");
        return address;
    }

    VkDeviceAddress SkinTables::writeWeights(const SceneTables& scene, const FrameSlot slot, const Index mesh)
    {
        assert(slot.get() < mSlots);

        const MeshRange& range = scene.mMeshes.getRows()[mesh];
        const VkDeviceSize at = VkDeviceSize{ range.mPoseOffset } * sizeof(float);
        mWeights[slot.get()].writeAt(at, scene.getMeshWeights(mesh));

        return mWeights[slot.get()].getDeviceAddress() + at;
    }

    VkDeviceAddress SkinTables::getBindPositions(const MeshRange& mesh) const
    {
        return mBindPositions.getDeviceAddress() + VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceAddress SkinTables::getBindNormals(const MeshRange& mesh) const
    {
        return mBindNormals.getDeviceAddress() + VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceAddress SkinTables::getRuns(const Rig& rig) const
    {
        return mRuns.getDeviceAddress() + VkDeviceSize{ rig.mRuns.mOffset } * sizeof(std::uint32_t);
    }

    VkDeviceAddress SkinTables::getInfluences(const Rig& rig) const
    {
        return mInfluences.getDeviceAddress() + VkDeviceSize{ rig.mInfluences.mOffset } * sizeof(Shaders::GpuInfluence);
    }

    VkDeviceAddress SkinTables::getMorphOffsets(const Morph& morph) const
    {
        return mMorphOffsets.getDeviceAddress() + VkDeviceSize{ morph.mOffsets.mOffset } * sizeof(osg::Vec3f);
    }

    VkDeviceSize SkinTables::getBytes() const
    {
        VkDeviceSize total = mBindPositions.getSize() + mBindNormals.getSize() + mRuns.getSize() + mInfluences.getSize()
            + mMorphOffsets.getSize();
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
            total += mBones[slot].getSize() + mWeights[slot].getSize();

        return total;
    }
}
