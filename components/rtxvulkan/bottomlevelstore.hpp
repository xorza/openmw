#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/runs.hpp>
#include <components/rtx/scratch.hpp>
#include <components/rtx/slots.hpp>

#include "blockedbuffer.hpp"
#include "owned.hpp"
#include "structurebuild.hpp"
#include "structurestorage.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class Graveyard;
    class GpuTimer;
    class SceneDesc;

    /// One bottom-level acceleration structure per mesh, all inside a single storage buffer at
    /// offsets, and the compaction that keeps them tight: a structure is built loose, the driver is
    /// asked what a tight copy would come to, and the copy takes room out of the same storage.
    class BottomLevelStore
    {
    public:
        /// @param slots how many frames may be tracing this scene at once, which is what the
        ///        compaction's readiness rule counts placements against.
        BottomLevelStore(const Device& device, std::uint32_t slots);
        ~BottomLevelStore();

        /// Creates and records the build of a structure for each of `meshes`, taking storage for it.
        /// A slot that already holds one has it destroyed first: a slot the scene handed out again
        /// arrives carrying different geometry.
        ///
        /// @param poses the first copy of the deforming vertices, which is what a deforming mesh's
        ///        structure is built over — `SkinPass` has written the pose into it.
        /// @param indices the shared index blocks, which every structure is built through.
        void build(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes, const BlockedBuffer& poses,
            const BlockedBuffer& indices, Graveyard& graveyard);

        /// Destroys the structures of `meshes` and gives their storage back. Idempotent, because
        /// both the frame that places and the one that appends run it. The structures go to
        /// `graveyard`: the last frame's top level still names them.
        void release(std::span<const Index> meshes, Graveyard& graveyard);

        std::size_t size() const { return mStructures.size(); }
        VkAccelerationStructureKHR getStructure(const Index mesh) const { return mStructures[mesh]; }
        VkDeviceAddress getAddress(const Index mesh) const { return mAddresses[mesh]; }

        /// Whether `mesh`'s structure was built with `ALLOW_UPDATE`, which is whether the scene's
        /// `MeshRange::mDeform` named a kind at the time it was built. A mesh's kind is fixed when
        /// it arrives, so this is also whether the mesh can ever be refitted.
        bool isUpdatable(const Index mesh) const { return mUpdatable[mesh] != 0; }

        /// What a refit of `mesh` asks for, so a frame does not have to ask the driver again.
        /// Nought for a mesh that was not built to be refitted.
        VkDeviceSize getUpdateScratch(const Index mesh) const { return mUpdateScratch[mesh]; }

        /// Reads every compaction answer whose placement has certainly run, and makes a tight
        /// structure for as many of the answered as this placement's budget takes. The set it
        /// returns names the meshes whose structures moved, whose rows the caller writes again.
        /// Counts the placement, which is what the readiness rule below reads; one call per
        /// placement.
        const SlotSet& prepareCompaction(Graveyard& graveyard);

        /// Copies each structure `prepareCompaction` made room for into it.
        void recordCompaction(VkCommandBuffer commands, GpuTimer* timer);

        /// The room the structures were given, and what they occupy in it. Neither counts the
        /// geometry they were built from.
        VkDeviceSize getBytes() const { return mStorage.getBytes(); }
        VkDeviceSize getLiveBytes() const { return mStorage.getLiveBytes(); }

        /// What the structures still to be copied tight would come to, or nought where there are
        /// none and where the device would not say — what is left to save, falling to nothing over
        /// the placements after an arrival. The answers already read, and not a question of its
        /// own: `VK_QUERY_RESULT_WAIT_BIT` would stand the CPU still on the frame a cell arrives in.
        VkDeviceSize getCompactableBytes() const { return mCompactableTight; }

        /// What those same structures occupy now. The pair says what compaction has left to give
        /// back.
        VkDeviceSize getCompactableNowBytes() const { return mCompactableNow; }

    private:
        /// What the compaction knows about the structure in a slot — a state per slot and a
        /// question per structure, asked once, because asking about every loose structure at every
        /// build never read an answer while cells kept arriving.
        enum class Tightness : std::uint8_t
        {
            /// No structure, or one that refits and so keeps its slack.
            None,

            /// Built loose and not yet asked about.
            Loose,

            /// Its question is in a batch the queue may not have reached.
            Asked,

            /// The driver's answer is in `mTightSize`, and the copy is owed.
            Answered,

            /// Copied tight, or no smaller tight: nothing more to do.
            Tight,
        };

        /// One question recorded, in the order they were, so the ones ready to read are a prefix.
        struct Ask
        {
            Index mSlot = sNoIndex;
            std::uint64_t mAt = 0;
        };

        /// What the compaction knows about the structure in one slot: where it stands, the placement
        /// count when its question was recorded — what `readAnswers` reads it against — what it was
        /// created at, and what the driver said a tight copy would come to, once answered.
        struct Compaction
        {
            Tightness mTightness = Tightness::None;
            std::uint64_t mAskedAt = 0;
            VkDeviceSize mBuiltSize = 0;
            VkDeviceSize mTightSize = 0;
        };

        /// Records the compaction question for every loose structure not yet asked about.
        void askWhatCompactionWouldSave(VkCommandBuffer commands, Graveyard& graveyard);

        /// Records the question for the run of consecutive slots gathered in `mAskScratch`, which
        /// starts at `first`, and empties it. Nothing where nothing was gathered.
        void askRun(VkCommandBuffer commands, std::uint32_t first);

        /// Reads every answer whose placement has certainly run.
        void readAnswers();

        /// Whether `ask` is still the question its slot is waiting on: a slot built again since is
        /// waiting on a later one.
        bool isOutstanding(const Ask& ask) const
        {
            const Compaction& state = mCompaction[ask.mSlot];
            return state.mTightness == Tightness::Asked && state.mAskedAt == ask.mAt;
        }

        /// Drops what the compaction knew about `slot`, ahead of its structure going.
        void forget(Index slot);

        const Device& mDevice;

        StructureStorage mStorage{ VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            "bottom level structures" };

        std::vector<VkAccelerationStructureKHR> mStructures;

        /// Where each of those sits in the storage, so a released mesh can give its room back.
        std::vector<StructureRoom> mRooms;

        /// Each of those structures' device address, asked for once when it was made: a
        /// nine-by-nine exterior asking per instance is fifty thousand driver calls a frame.
        std::vector<VkDeviceAddress> mAddresses;

        std::vector<VkDeviceSize> mUpdateScratch;
        std::vector<std::uint8_t> mUpdatable;

        /// What one run of `build` describes.
        StructureBuildBatch mBuild;

        /// One mesh of the run `build` was handed: how big its structure comes out, where in the one
        /// scratch buffer they share its build takes its working room, and where its vertices sit in
        /// the buffer `build` stages them into. All three are filled in one pass and read in the
        /// next, so they are one row. The staging offset means nothing for a mesh that deforms,
        /// which is built from its pose.
        struct BuildRow
        {
            VkDeviceSize mSize = 0;
            VkDeviceSize mScratchOffset = 0;
            VkDeviceSize mArrivedAt = 0;
        };

        /// Refilled per `build`, one row per mesh handed in, in that order.
        std::vector<BuildRow> mBuilding;

        /// The builds actually recorded, which is `mBuild.mBuilds` without the meshes that came out
        /// at nought bytes — a mesh with no triangles is described by nobody and built by nobody.
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mLiveBuilds;

        /// The compaction's state per slot, grown with the mesh table.
        std::vector<Compaction> mCompaction;

        /// One query per slot, grown with the mesh table. The pool it outgrows is buried and not
        /// destroyed — a batch in flight may still be writing into it — and whoever was asked
        /// through it is asked again through the new one.
        Owned<VkQueryPool, vkDestroyQueryPool> mCompactable;
        std::uint32_t mCompactablePool = 0;

        /// The questions outstanding, oldest first, and the slots answered and not yet copied.
        Backlog<Ask> mAsked;
        Backlog<Index> mAnswered;

        /// One run of consecutive slots' handles, and one run's answers. Refilled per run.
        std::vector<VkAccelerationStructureKHR> mAskScratch;
        std::vector<VkDeviceSize> mReadScratch;

        /// What the answered structures occupy as they stand, and what they would come to tight,
        /// so the pair the report prints is a saving rather than a number on its own.
        VkDeviceSize mCompactableNow = 0;
        VkDeviceSize mCompactableTight = 0;

        /// What this placement copies, refilled each time. Kept so a compaction allocates nothing.
        std::vector<VkCopyAccelerationStructureInfoKHR> mCompactionCopies;

        /// The meshes those copies moved, for the caller's walk over the rows placing them.
        SlotSet mMovedMeshes;

        /// How many placements this store has been through — what stands in for a fence: the ring
        /// waits for the frame `mSlots` back before it records this one, so a placement that far
        /// behind has finished on the queue.
        std::uint64_t mPlacements = 0;
        std::uint32_t mSlots = 1;
    };
}
