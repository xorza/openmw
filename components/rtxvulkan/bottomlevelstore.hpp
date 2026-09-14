#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/runs.hpp>
#include <components/rtx/slots.hpp>

#include "accelerationstructure.hpp"
#include "blockedbuffer.hpp"
#include "handles.hpp"
#include "structurebuild.hpp"
#include "structurestorage.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class Graveyard;
    class GpuTimer;
    class SceneDesc;

    /// A list consumed from the front in the order it was filled, emptied once it is drained and
    /// never before, so what is left in it is never moved.
    template <class T>
    struct Backlog
    {
        std::vector<T> mItems;
        std::size_t mRead = 0;

        std::size_t size() const { return mItems.size() - mRead; }
        bool empty() const { return size() == 0; }
        const T& at(std::size_t offset) const { return mItems[mRead + offset]; }
        void push(const T& item) { mItems.push_back(item); }
        void pop(std::size_t count) { mRead += count; }

        /// Lets go of what was consumed, where everything was.
        void settle()
        {
            if (mRead == mItems.size())
            {
                mItems.clear();
                mRead = 0;
            }
        }
    };

    /// One bottom-level acceleration structure per mesh, all inside a single storage buffer at
    /// offsets, and the compaction that keeps them tight: a structure is built loose, the driver is
    /// asked what a tight copy would come to, and the copy takes room out of the same storage.
    class BottomLevelStore
    {
    public:
        explicit BottomLevelStore(const Device& device);

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

        std::size_t size() const { return mRows.size(); }
        VkAccelerationStructureKHR getStructure(const Index mesh) const { return mRows[mesh].mStructure.getHandle(); }
        VkDeviceAddress getAddress(const Index mesh) const { return mRows[mesh].mStructure.getAddress(); }

        /// Whether `mesh`'s structure was built with `ALLOW_UPDATE`, which is whether the scene's
        /// `MeshRange::mDeform` named a kind at the time it was built. A mesh's kind is fixed when
        /// it arrives, so this is also whether the mesh can ever be refitted.
        bool isUpdatable(const Index mesh) const { return mRows[mesh].mUpdatable; }

        /// What a refit of `mesh` asks for, so a frame does not have to ask the driver again.
        /// Nought for a mesh that was not built to be refitted.
        VkDeviceSize getUpdateScratch(const Index mesh) const { return mRows[mesh].mUpdateScratch; }

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
        /// `mAt` is the timeline value the batch it was recorded into rides.
        struct Ask
        {
            Index mSlot = sNoIndex;
            std::uint64_t mAt = 0;
        };

        /// What the compaction knows about the structure in one slot: where it stands, the timeline
        /// value its question rides — what `readAnswers` reads it against — what it was created at,
        /// and what the driver said a tight copy would come to, once answered.
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

        /// Reads every answer whose submit the timeline says has run.
        void readAnswers();

        /// Whether `ask` is still the question its slot is waiting on: a slot built again since is
        /// waiting on a later one.
        bool isOutstanding(const Ask& ask) const
        {
            const Compaction& state = mRows[ask.mSlot].mCompaction;
            return state.mTightness == Tightness::Asked && state.mAskedAt == ask.mAt;
        }

        /// Buries `slot`'s structure and forgets what the compaction knew about it, ahead of the
        /// slot being built again or given back. Idempotent: a slot holding nothing buries nothing.
        void retire(Index slot, Graveyard& graveyard);

        /// One mesh slot: its structure, in its room, and what the refit and the compaction know
        /// about it. One row and not six lists, so a slot cannot be half updated.
        struct Row
        {
            AccelerationStructure mStructure;

            /// What a refit asks for, kept so a frame does not ask the driver again. Nought for a
            /// mesh not built to be refitted.
            VkDeviceSize mUpdateScratch = 0;

            /// Whether the structure was built with `ALLOW_UPDATE`.
            bool mUpdatable = false;

            Compaction mCompaction;
        };

        const Device& mDevice;

        // Before the rows, which give their rooms back to it as they go.
        StructureStorage mStorage{ sStructureStorageUsage, "bottom level structures" };

        /// One row per mesh slot, grown with the mesh table.
        std::vector<Row> mRows;

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

        /// One query per slot, grown with the mesh table. The pool it outgrows is buried and not
        /// destroyed — a batch in flight may still be writing into it — and whoever was asked
        /// through it is asked again through the new one.
        QueryPool mCompactable;
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
    };
}
