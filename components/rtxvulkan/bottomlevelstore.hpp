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
    class SceneMicromaps;
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
        void build(Batch& batch, const SceneTables& scene, std::span<const Index> meshes,
            const SceneMicromaps& micromaps, const BlockedBuffer& poses, const BlockedBuffer& indices,
            Graveyard& graveyard);

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

        /// Whether `mesh`'s structure was built over a micromap, which is what a row placing it
        /// counts by and what a refit of it has to describe again.
        bool isMicromapped(const Index mesh) const { return mMicromapped[mesh] != 0; }

        /// What a refit of `mesh` asks for, so a frame does not have to ask the driver again.
        /// Nought for a mesh that was not built to be refitted.
        VkDeviceSize getUpdateScratch(const Index mesh) const { return mUpdateScratch[mesh]; }

        /// Reads the compaction answers, once the placement that asked for them has certainly run,
        /// and makes a tight structure for as many as this placement's budget takes.
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
        /// **Every structure the scene holds that is still loose**, asked afresh whenever anything
        /// is built: a route that builds at every crossing would otherwise report whatever the last
        /// crossing brought, which is nought where it brought only actors.
        VkDeviceSize getCompactableBytes() const;

        /// What those same structures occupy now. The pair says what compaction has left to give
        /// back.
        VkDeviceSize getCompactableNowBytes() const { return mCompactableNow; }

    private:
        /// No placement, which is what `mQueriedAt` holds while nothing has been asked.
        static constexpr std::uint64_t sNoPlacement = ~std::uint64_t{ 0 };

        /// Writes what a tight copy of each structure the last build made would come to.
        void askWhatCompactionWouldSave(VkCommandBuffer commands);

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
        std::vector<std::uint8_t> mMicromapped;

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

        /// One query per compactable structure — every built mesh that does not refit — holding
        /// what a tight copy of it would come to.
        ///
        /// Made again when the scene outgrows it, which loses what it held: a figure is a figure
        /// about the build that wrote it.
        Owned<VkQueryPool, vkDestroyQueryPool> mCompactable;

        /// How many queries the pool holds, and how many the last build wrote. The first only grows.
        std::uint32_t mCompactablePool = 0;
        std::uint32_t mCompactableCount = 0;

        /// What the structures the last question named occupy as they stand, so the pair the report
        /// prints is a saving rather than a number on its own.
        VkDeviceSize mCompactableNow = 0;

        /// What each mesh's structure was created at, by slot.
        std::vector<VkDeviceSize> mBuiltSize;

        /// Refilled per build, so the walk that gathers them allocates nothing.
        std::vector<VkAccelerationStructureKHR> mCompactableHandles;

        /// The mesh each of those belongs to. Beside the handles because the pool is packed over
        /// what is compactable, so a query's index is not a mesh slot.
        std::vector<Index> mCompactableSlots;

        /// What the driver said each would come to, and how far through them the copies have got.
        /// Empty where nothing is outstanding.
        std::vector<VkDeviceSize> mCompactedSizes;
        std::size_t mCompactionAt = 0;

        /// Whether each mesh's structure has already been copied tight, so the next build's question
        /// passes over it and nothing is copied twice. Cleared where a slot is built again.
        std::vector<std::uint8_t> mCompacted;

        /// What this placement copies, refilled each time. Kept so a compaction allocates nothing.
        std::vector<VkCopyAccelerationStructureInfoKHR> mCompactionCopies;

        /// The meshes those copies moved, for the caller's walk over the rows placing them.
        SlotSet mMovedMeshes;

        /// How many placements this store has been through, and which one asked the compaction
        /// questions — `sNoPlacement` where none are outstanding.
        ///
        /// **What stands in for a fence.** The ring waits for the frame `mSlots` back before it
        /// records this one, so a placement that far behind has finished on the queue and its
        /// answers are there to be read. Asking with `WAIT_BIT` instead would stall the frame a
        /// cell arrives in, which is the one frame that can least afford it.
        std::uint64_t mPlacements = 0;
        std::uint64_t mQueriedAt = sNoPlacement;

        std::uint32_t mSlots = 1;
    };
}
