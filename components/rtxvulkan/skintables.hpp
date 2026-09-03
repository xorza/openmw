#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/scenedesc.hpp>

#include "buffer.hpp"
#include "frameslots.hpp"

namespace Rtx
{
    class Device;
    class Graveyard;

    /// What one scene's deforming meshes are posed from: their bind poses, their rigs and morphs,
    /// and the rows and weights a frame writes.
    ///
    /// **The bind poses live here and not in the shared vertex blocks**, because those blocks are the
    /// pose's destination: a skinned mesh's run in a slot's copy of the positions and the normals is
    /// what the kernel writes and what the refit and the hit then read. What it reads from has to be
    /// somewhere the frame never writes, and it is a few bodies' worth against a cell — so it is a
    /// table of the deforming meshes alone, indexed by `MeshRange::mBindOffset`.
    ///
    /// **Plain buffers, grown by doubling and rewritten whole when they grow.** Nothing keeps an
    /// address into these across a frame: the kernel is handed each run's address in its push
    /// constants, so a table that moved is a table handed over at its new address. A growth is an
    /// arrival, which waits every frame out first.
    ///
    /// **The rows and the weights are per frame in flight and never rewritten on growth.** A
    /// mesh's rows are written and read in the same placement — `SkinPass::record` writes them and
    /// dispatches — so a copy holds nothing a later frame reads, and the copy the other frame is
    /// reading is the other slot's.
    class SkinTables
    {
    public:
        /// @param slots how many frames may be posing this scene at once: `sFrameSlots` for the
        ///        world, one for a picture inside the interface.
        SkinTables(const Device& device, const SceneDesc& scene, std::uint32_t slots, Graveyard& graveyard);

        SkinTables(const SkinTables&) = delete;
        SkinTables& operator=(const SkinTables&) = delete;

        /// Takes in what the scene says arrived: the bind poses of the deforming meshes, the rigs
        /// and the morphs. **With nothing in flight**, which the caller guarantees.
        void extend(const SceneDesc& scene, Graveyard& graveyard);

        /// Writes `mesh`'s rows into `slot`'s copy and returns where they landed, for the dispatch
        /// about to read them. A `hostWritten` copy, so the write is a `memcpy` and the submit that
        /// follows sees it.
        VkDeviceAddress writeBones(const SceneDesc& scene, std::uint32_t slot, Index mesh);

        /// The same for a morphed mesh's weights.
        VkDeviceAddress writeWeights(const SceneDesc& scene, std::uint32_t slot, Index mesh);

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
        void writeBind(const SceneDesc& scene, std::span<const Index> meshes, bool whole);
        void writeRigs(const SceneDesc& scene, std::span<const Index> rigs, bool whole);
        void writeMorphs(const SceneDesc& scene, std::span<const Index> morphs, bool whole);

        const Device* mDevice = nullptr;
        std::uint32_t mSlots = 1;

        Buffer mBindPositions;
        Buffer mBindNormals;
        Buffer mRuns;
        Buffer mInfluences;
        Buffer mMorphOffsets;

        std::array<Buffer, sFrameSlots> mBones;
        std::array<Buffer, sFrameSlots> mWeights;

        /// Every mesh, rig or morph, for a table written whole. Kept so a growth allocates nothing
        /// of its own.
        std::vector<Index> mEvery;
    };
}
