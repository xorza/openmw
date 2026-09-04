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
        const Device& device, const SceneDesc& scene, const std::uint32_t slots, Graveyard& graveyard)
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

    void SkinTables::extend(const SceneDesc& scene, Graveyard& graveyard)
    {
        const Device& device = *mDevice;

        // **Grown to what the scene reaches, and written whole where a growth moved it.** The
        // arrivals are what a frame with an actor walking in costs; a table made again is what a
        // cell full of them costs, once per doubling.
        const VkDeviceSize bind = VkDeviceSize{ scene.getBindVertexCount() } * sizeof(osg::Vec3f);
        const bool bindMoved = outgrow(mBindPositions, device, bind, sTableUsage, graveyard)
            | outgrow(mBindNormals, device, bind, sTableUsage, graveyard);
        writeBind(scene, scene.getArrivedMeshes(), bindMoved);

        const bool rigsMoved
            = outgrow(mRuns, device, scene.getRuns().size() * sizeof(std::uint32_t), sTableUsage, graveyard)
            | outgrow(mInfluences, device, scene.getInfluences().size() * sizeof(Shaders::GpuInfluence), sTableUsage,
                graveyard);
        writeRigs(scene, scene.getArrivedRigs(), rigsMoved);

        const bool morphsMoved = outgrow(
            mMorphOffsets, device, scene.getMorphOffsets().size() * sizeof(osg::Vec3f), sTableUsage, graveyard);
        writeMorphs(scene, scene.getArrivedMorphs(), morphsMoved);

        // Nothing is written into these on arrival: a mesh's rows reach a copy in the placement
        // that dispatches over them, and not before.
        for (std::uint32_t slot = 0; slot < mSlots; ++slot)
        {
            outgrow(mBones[slot], device, scene.getBones().size() * sizeof(Shaders::GpuBone), sTableUsage, graveyard);
            outgrow(mWeights[slot], device, scene.getWeights().size() * sizeof(float), sTableUsage, graveyard);
        }
    }

    void SkinTables::writeBind(const SceneDesc& scene, std::span<const Index> meshes, const bool whole)
    {
        const std::span<const MeshRange> ranges = scene.getMeshes();
        for (const Index index : whole ? everyBelow(ranges.size(), mEvery) : meshes)
        {
            const MeshRange& mesh = ranges[index];
            if (mesh.mDeform == Deform::None || mesh.mVertexCount == 0)
                continue;

            const VkDeviceSize at = VkDeviceSize{ mesh.mBindOffset } * sizeof(osg::Vec3f);
            mBindPositions.writeAt(at, scene.getMeshPositions(index));
            mBindNormals.writeAt(at, scene.getNormals().subspan(mesh.mVertexOffset, mesh.mVertexCount));
        }
    }

    void SkinTables::writeRigs(const SceneDesc& scene, std::span<const Index> rigs, const bool whole)
    {
        const std::span<const Rig> table = scene.getRigs();
        for (const Index index : whole ? everyBelow(table.size(), mEvery) : rigs)
        {
            // A freed slot skins nothing and holds no run to write.
            const Rig& rig = table[index];
            if (rig.mVertexCount == 0)
                continue;

            mRuns.writeAt(VkDeviceSize{ rig.mRunOffset } * sizeof(std::uint32_t),
                scene.getRuns().subspan(rig.mRunOffset, rig.mVertexCount));
            mInfluences.writeAt(VkDeviceSize{ rig.mInfluenceOffset } * sizeof(Shaders::GpuInfluence),
                scene.getInfluences().subspan(rig.mInfluenceOffset, rig.mInfluenceCount));
        }
    }

    void SkinTables::writeMorphs(const SceneDesc& scene, std::span<const Index> morphs, const bool whole)
    {
        const std::span<const Morph> table = scene.getMorphs();
        for (const Index index : whole ? everyBelow(table.size(), mEvery) : morphs)
        {
            const Morph& morph = table[index];
            if (morph.mVertexCount == 0)
                continue;

            mMorphOffsets.writeAt(VkDeviceSize{ morph.mOffsetsAt } * sizeof(osg::Vec3f),
                scene.getMorphOffsets().subspan(morph.mOffsetsAt, morph.mTargetCount * morph.mVertexCount));
        }
    }

    VkDeviceAddress SkinTables::writeBones(const SceneDesc& scene, const std::uint32_t slot, const Index mesh)
    {
        assert(slot < mSlots);

        const MeshRange& range = scene.getMeshes()[mesh];
        const VkDeviceSize at = VkDeviceSize{ range.mPoseOffset } * sizeof(Shaders::GpuBone);
        mBones[slot].writeAt(at, scene.getMeshBones(mesh));

        const VkDeviceAddress address = mBones[slot].getDeviceAddress() + at;
        assert(address % sBoneAlignment == 0 && "a run of rows the kernel's reference claims more of than is true");
        return address;
    }

    VkDeviceAddress SkinTables::writeWeights(const SceneDesc& scene, const std::uint32_t slot, const Index mesh)
    {
        assert(slot < mSlots);

        const MeshRange& range = scene.getMeshes()[mesh];
        const VkDeviceSize at = VkDeviceSize{ range.mPoseOffset } * sizeof(float);
        mWeights[slot].writeAt(at, scene.getMeshWeights(mesh));

        return mWeights[slot].getDeviceAddress() + at;
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
        return mRuns.getDeviceAddress() + VkDeviceSize{ rig.mRunOffset } * sizeof(std::uint32_t);
    }

    VkDeviceAddress SkinTables::getInfluences(const Rig& rig) const
    {
        return mInfluences.getDeviceAddress() + VkDeviceSize{ rig.mInfluenceOffset } * sizeof(Shaders::GpuInfluence);
    }

    VkDeviceAddress SkinTables::getMorphOffsets(const Morph& morph) const
    {
        return mMorphOffsets.getDeviceAddress() + VkDeviceSize{ morph.mOffsetsAt } * sizeof(osg::Vec3f);
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
