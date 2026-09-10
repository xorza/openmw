#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/index.hpp>
#include <components/rtx/slotset.hpp>

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
    struct SceneTables;

    /// One bottom-level acceleration structure per mesh, and what it takes to keep them tight.
    ///
    /// All of them sit inside a single storage buffer at offsets. Per-mesh buffers would be the
    /// obvious shape and would spend a device allocation on each of a cell's several hundred meshes;
    /// the scene description is flat for the same reason.
    ///
    /// **The compaction is here because it is what a store does**, and not a stage beside one. A
    /// structure is built loose, the driver is asked what a tight copy would come to, and the copy
    /// takes room out of the same storage and gives the loose room back — every step of which reads
    /// the handles, the rooms, the addresses and the built sizes this holds and nothing else.
    class BottomLevelStore
    {
    public:
        /// @param slots how many frames may be tracing this scene at once, which is what the
        ///        compaction's readiness rule counts placements against.
        BottomLevelStore(const Device& device, std::uint32_t slots);
        ~BottomLevelStore();

        BottomLevelStore(const BottomLevelStore&) = delete;
        BottomLevelStore& operator=(const BottomLevelStore&) = delete;

        /// Creates and records the build of a structure for each of `meshes`, taking storage for it.
        ///
        /// A slot that already holds one has it destroyed and its room given back first: a slot the
        /// scene took back and handed out again arrives carrying different geometry.
        ///
        /// @param poses the first copy of the deforming vertices, which is what a deforming mesh's
        ///        structure is built over — `SkinPass` has written the pose into it.
        /// @param indices the shared index blocks, which every structure is built through.
        void build(Batch& batch, const SceneTables& scene, std::span<const Index> meshes, const BlockedBuffer& poses,
            const BlockedBuffer& indices, Graveyard& graveyard);

        /// Destroys the structures of `meshes` and gives their storage back.
        ///
        /// **Idempotent**, because both the frame that places and the one that appends run it: a
        /// slot whose structure has already gone holds no handle and no room, and asking again is a
        /// pair of comparisons.
        ///
        /// The structures go to `graveyard` rather than being destroyed: the last frame's top level
        /// still names them, and that frame may still be tracing.
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
        /// structure for as many of the answered as this placement's budget takes.
        ///
        /// The set it returns names the meshes whose structures moved: every row placing one names
        /// an address that is no longer there, and the caller writes those rows again. Empty where
        /// nothing was copied, which is also when `recordCompaction` has nothing to record.
        ///
        /// **Counts the placement**, which is what the readiness rule below reads. One call per
        /// placement, which is what every caller makes.
        const SlotSet& prepareCompaction(Graveyard& graveyard);

        /// Copies each structure `prepareCompaction` made room for into it.
        void recordCompaction(VkCommandBuffer commands, GpuTimer* timer);

        /// The room the structures were given, and what they occupy in it. Neither counts the
        /// geometry they were built from.
        VkDeviceSize getBytes() const { return mStorage.getBytes(); }
        VkDeviceSize getLiveBytes() const { return mStorage.getLiveBytes(); }

        /// What the structures still to be copied tight would come to, or nought where there are
        /// none and where the device would not say.
        ///
        /// **What is left to save, and so nought once a cell has settled.** A structure is built
        /// loose because the builder cannot know the answer until it has finished, and
        /// `prepareCompaction` copies each into the size it turned out to need at a budget per
        /// placement. So this falls to nothing over the placements after an arrival, while
        /// `getBytes` falls by what it named.
        ///
        /// **The answers already read, and not a question of its own.** The queries are read where
        /// `prepareCompaction` reads them, `mSlots` placements after the one that wrote them — asking
        /// here instead means `VK_QUERY_RESULT_WAIT_BIT`, which stands the CPU still until the builds
        /// this frame recorded have run. That is the frame a cell arrives in, and it is the one frame
        /// that can least afford it: measured at 3.5 ms an arrival, half of what the hand-over cost.
        VkDeviceSize getCompactableBytes() const { return mCompactableTight; }

        /// What those same structures occupy now. The pair says what compaction has left to give
        /// back.
        VkDeviceSize getCompactableNowBytes() const { return mCompactableNow; }

    private:
        /// What the compaction knows about the structure in a slot.
        ///
        /// **A state per slot and a question per structure, asked once.** This used to ask about
        /// every loose structure the scene held at every build and read the answers only when no
        /// build followed within `mSlots` placements — so on a route that builds on every frame the
        /// answers were never read, nothing was ever copied tight, and each build asked the device
        /// about thousands of structures over again: eight to thirteen milliseconds of device time
        /// on an arrival frame, in front of the trace, for two structures actually built.
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

        /// A list consumed from the front in the order it was filled.
        ///
        /// **Emptied once it is drained and never before**, so a route allocates for it only while
        /// it grows, and what is left in it is never moved.
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
            return mTightness[ask.mSlot] == Tightness::Asked && mAskedAt[ask.mSlot] == ask.mAt;
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

        /// Each of those structures' device address, asked for once when it was made.
        ///
        /// **Not once per instance per frame, which is what this replaced.** A handle lasts from one
        /// `setScene` to the next and its address with it, so a nine-by-nine exterior was making
        /// fifty thousand driver calls a frame to be told the same fifty thousand numbers.
        std::vector<VkDeviceAddress> mAddresses;

        std::vector<VkDeviceSize> mUpdateScratch;
        std::vector<std::uint8_t> mUpdatable;

        /// What one run of `build` describes.
        StructureBuildBatch mBuild;

        /// How big each mesh's structure comes out, and where in the one scratch buffer they share
        /// its build takes its working room. Beside each other because both are filled in the same
        /// pass and read in the next.
        std::vector<VkDeviceSize> mBuildSizes;
        std::vector<VkDeviceSize> mBuildScratchOffsets;

        /// Where each arriving static mesh's vertices sit in the buffer `build` stages them into, in
        /// bytes. Meaningless for a mesh that deforms, which is built from its pose.
        std::vector<VkDeviceSize> mArrivedAt;

        /// The builds actually recorded, which is `mBuild.mBuilds` without the meshes that came out
        /// at nought bytes — a mesh with no triangles is described by nobody and built by nobody.
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mLiveBuilds;

        /// What each mesh's structure was created at, by slot.
        std::vector<VkDeviceSize> mBuiltSize;

        /// Per slot: where its structure stands with the compaction, the placement count when its
        /// question was recorded — what `readAnswers` reads it against — and what the driver said a
        /// tight copy would come to, once answered.
        std::vector<Tightness> mTightness;
        std::vector<std::uint64_t> mAskedAt;
        std::vector<VkDeviceSize> mTightSize;

        /// One query per slot, grown with the mesh table.
        ///
        /// **Indexed by slot, so a question needs no bookkeeping of where its answer went.** The
        /// pool it outgrows is buried and not destroyed — a batch in flight may still be writing into
        /// it — and whoever was asked through it is asked again through the new one.
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

        /// How many placements this store has been through.
        ///
        /// **What stands in for a fence.** The ring waits for the frame `mSlots` back before it
        /// records this one, so a placement that far behind has finished on the queue and the
        /// answers it carried are there to be read. Asking with `WAIT_BIT` instead would stall the
        /// frame a cell arrives in, which is the one frame that can least afford it.
        std::uint64_t mPlacements = 0;
        std::uint32_t mSlots = 1;
    };
}
