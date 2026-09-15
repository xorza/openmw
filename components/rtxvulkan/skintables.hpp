#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/deformertable.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>

#include "buffer.hpp"
#include "frameslots.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class Graveyard;

    /// What one scene's deforming meshes are posed from: their bind poses, their rigs and morphs,
    /// and the rows and weights a frame writes. The bind poses live here and not in the shared
    /// vertex blocks, because those blocks are the pose's destination and what it reads from has
    /// to be somewhere the frame never writes — a table of the deforming meshes alone, indexed by
    /// `MeshRange::mBindOffset`. Plain buffers grown by doubling, because the kernel is handed each
    /// run's address in its push constants and a growth is an arrival, which buries the table it
    /// displaced. Device memory, written on the queue and by nothing else. The rows and the
    /// weights are per frame in flight and host-written, because `SkinPass::record` writes and
    /// reads them in one placement, and never rewritten on growth.
    class SkinTables
    {
    public:
        /// @param batch what the first arrival rides, which is every deforming mesh the scene holds.
        /// @param slots how many frames may be posing this scene at once.
        SkinTables(
            const Device& device, Graveyard& graveyard, Batch& batch, const SceneDesc& scene, std::uint32_t slots);

        /// Takes in what the scene says arrived: the bind poses of the deforming meshes, the rigs
        /// and the morphs, and the arrived meshes' rows into the first copy, which is the one an
        /// arrival is posed into. Through `batch` and not from the host: a run an arrival was
        /// given may be one a mesh that went held until the last sweep, and the frame that posed
        /// that mesh can still be on the queue reading it — a copy recorded now runs behind that
        /// frame, where a host write would land under it. Ends in the barrier the dispatch over the
        /// arrivals needs.
        void extend(Batch& batch, const SceneDesc& scene);

        /// Waits until nothing on the queue reads or writes `slot`'s rows and weights, ahead of a
        /// placement that writes them from the host. The reader is usually the placement before
        /// last's dispatch, long finished; after an arrival it is `extend`'s staged rows and the
        /// pose over them, carried by whatever submit came next. Each copy carries the value, so
        /// the wait is for that submit and not for the frame behind it.
        void finishReads(FrameSlot slot) const;

        /// Writes `mesh`'s rows into `slot`'s copy and returns where they landed, for the dispatch
        /// about to read them. A `hostWritten` copy, so the write is a `memcpy` and the submit that
        /// follows sees it — and a placement's copy, whose last reader the caller waited for, which
        /// the write asserts.
        VkDeviceAddress writeBones(const SceneDesc& scene, FrameSlot slot, Index mesh);

        /// The same for a morphed mesh's weights.
        VkDeviceAddress writeWeights(const SceneDesc& scene, FrameSlot slot, Index mesh);

        /// Where `mesh`'s rows sit in `slot`'s copy, for a dispatch over rows `extend` staged.
        VkDeviceAddress getBones(const MeshRange& mesh, FrameSlot slot) const;
        VkDeviceAddress getWeights(const MeshRange& mesh, FrameSlot slot) const;

        /// Where `mesh`'s bind pose starts, in each of the two bind tables.
        VkDeviceAddress getBindPositions(const MeshRange& mesh) const;
        VkDeviceAddress getBindNormals(const MeshRange& mesh) const;

        /// Where a rig's runs and influences start, and where a morph's offsets do.
        VkDeviceAddress getRuns(const Rig& rig) const;
        VkDeviceAddress getInfluences(const Rig& rig) const;
        VkDeviceAddress getMorphOffsets(const Morph& morph) const;

        VkDeviceSize getBytes() const;

    private:
        /// Writes the bind poses of `meshes` — or of every deforming mesh, where a table was made
        /// again — and the runs of `rigs` and `morphs` likewise.
        void writeBind(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes, bool whole);
        void writeRigs(Batch& batch, const SceneDesc& scene, std::span<const Index> rigs, bool whole);
        void writeMorphs(Batch& batch, const SceneDesc& scene, std::span<const Index> morphs, bool whole);

        /// Stages the rows or the weights of `meshes` into the first copy.
        void writeRows(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes);

        const Device* mDevice = nullptr;

        /// Where a table this outgrows goes, held until the frames still reading it have run.
        Graveyard* mGraveyard = nullptr;

        Buffer mBindPositions;
        Buffer mBindNormals;
        Buffer mRuns;
        Buffer mInfluences;
        Buffer mMorphOffsets;

        PerSlot<Buffer> mBones;
        PerSlot<Buffer> mWeights;

        /// Every mesh, rig or morph, for a table written whole. Kept so a growth allocates nothing
        /// of its own.
        std::vector<Index> mEvery;
    };
}
