#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/refusal.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/shaders/scene.h>

#include "accelerationstructure.hpp"
#include "blockedbuffer.hpp"
#include "bottomlevelstore.hpp"
#include "buffer.hpp"
#include "frameslots.hpp"
#include "placing.hpp"
#include "slottable.hpp"
#include "structurebuild.hpp"

namespace Rtx
{
    class Batch;
    class GpuTimer;
    class Device;
    class SceneDesc;

    /// The neutral transform in Vulkan's storage: three rows of four, which is exactly what
    /// `Transform3x4` holds. The transposition that matters happened in `toTransform3x4`, once.
    VkTransformMatrixKHR toVulkanTransform(const Transform3x4& transform);

    /// Every acceleration structure a scene needs: the top level over the instances, the refit that
    /// keeps a deforming mesh's structure over its pose, and the rows both are built from. The
    /// bottom levels themselves are `BottomLevelStore`'s.
    class SceneAcceleration
    {
    public:
        /// Writes every mesh's geometry and every row, and builds nothing yet, because a pose comes
        /// between: `SkinPass` writes it into the first copy of the positions, and `build` then
        /// builds over what the frame will trace. A structure built over the bind and refitted into
        /// the pose would keep the bind's shape for the life of the mesh.
        ///
        /// @param slots how many frames may be tracing this scene at once, which is how many copies
        ///        there are of the rows and of the positions a refit reads.
        SceneAcceleration(const Device& device, Batch& batch, const SceneDesc& scene, std::uint32_t slots);

        /// Builds every mesh's structure, writes every row, and builds the top level, in one submit
        /// with each stage ending in the barrier the next one needs. Once, after the constructor.
        /// `scene` must place at least one instance: a top-level structure over nothing has no
        /// instance buffer to be built from. A mesh the device has no room for is left out and
        /// appended to `refused` — `BottomLevelStore::build`.
        void build(Batch& batch, const SceneDesc& scene, std::span<const InstanceRecord> records,
            std::vector<Refusal>& refused);

        /// Rebuilds what a moved world changed: every deformed mesh's structure, then the top level,
        /// in one command buffer with a barrier between — two `submitAndWait`s were a round trip
        /// through the driver in the middle of the frame for a dependency a barrier expresses. A
        /// deforming mesh's structure is refitted over the vertices `SkinPass` wrote into `slot`'s
        /// copy, which its `ALLOW_UPDATE` bit costs that mesh alone a larger structure for. Skipped
        /// whole where nothing moved and nothing deformed. Recorded into `placing.mCommands` and not
        /// submitted, so the caller decides whether the queue is asked now or with the frame; true
        /// where anything was recorded.
        ///
        /// @param changed the slots `updateInstanceRecords` wrote, which is the one list any of
        ///        this is driven by. `records` is handed in rather than made here because
        ///        `SceneBuffers` needs the same rows.
        bool place(const SceneDesc& scene, std::span<const InstanceRecord> records, std::span<const Index> changed,
            const Placing& placing);

        /// Waits until no build on the queue reads `slot`'s copy of the rows, ahead of the
        /// placement that writes it.
        void finishReads(FrameSlot slot) const { mRowTable.finishReads(slot); }

        /// Takes in the geometry of the meshes the scene says arrived and lets go of the ones it says
        /// went. `buildArrived` builds their structures, once the pass has posed them. Every
        /// structure already built stays where it is, and the top level picks the change up for
        /// nothing. Safe with frames in flight, because nothing it writes is room one of them
        /// holds; `check` under synchronization validation is what says so.
        void extend(Batch& batch, const SceneDesc& scene);

        /// Builds the structures of the meshes that arrived, over the first copy of the positions
        /// as `extend` and the pass left it. A mesh the device has no room for is left out and
        /// appended to `refused`, as `build` does.
        ///
        /// @param timer the frame the arrival lands in, so its builds are one zone of that frame's
        ///        report rather than device time nothing accounts for. Null for a picture inside the
        ///        interface, which is not timed — `VulkanRenderer::placeScene` says why.
        void buildArrived(Batch& batch, const SceneDesc& scene, GpuTimer* timer, std::vector<Refusal>& refused);

        /// Destroys the structures of `meshes` and gives their storage back.
        void release(std::span<const Index> meshes) { mBottomLevel.release(meshes); }

        VkAccelerationStructureKHR getTopLevel() const { return mTopLevel.getHandle(); }

        /// Where the index blocks are, as a shader reads them at a hit. Here rather than in
        /// `SceneBuffers` because the build had to have them first. The address of a table of
        /// addresses, because the indices are a list of blocks a shader resolves
        /// `block[id / INDEX_BLOCK]` in itself.
        VkDeviceAddress getIndexBlocks() const { return mIndices.getTableAddress(); }

        /// The poses, for the pass that writes a deforming mesh's vertices into a slot's copy of
        /// them — and their account, which is what tells that pass which meshes each copy owes.
        SlotBlocks& getPoses() { return mPoses; }

        /// Where `slot`'s copy keeps its poses, as a shader reads them at a hit: every deforming
        /// mesh as this frame traces it. And the copy `slot` does not trace, which by the account
        /// `SlotBlocks` keeps is every deforming mesh as the previous frame traced it —
        /// `GpuTables::mPreviousPoseBlocks` says why.
        VkDeviceAddress getPoseBlocks(const FrameSlot slot) const { return mPoses.at(slot).getTableAddress(); }
        VkDeviceAddress getPreviousPoseBlocks(const FrameSlot slot) const
        {
            static_assert(sFrameSlots == 2, "the copy a frame does not trace is the previous frame's only with two");
            return mPoses.at(slot.next()).getTableAddress();
        }

        /// The room the structures were given, and what they occupy in it — a pair, because a
        /// structure copied tight gives its loose room back and a block is returned to the device
        /// only when nothing is left in it. Neither counts the geometry they were built from.
        VkDeviceSize getStructureBytes() const { return mBottomLevel.getBytes() + mTopLevelBytes; }
        VkDeviceSize getStructureLiveBytes() const { return mBottomLevel.getLiveBytes() + mTopLevelBytes; }

        VkDeviceSize getCompactableBytes() const { return mBottomLevel.getCompactableBytes(); }
        VkDeviceSize getCompactableNowBytes() const { return mBottomLevel.getCompactableNowBytes(); }

        /// How many placements a refitted structure is left to its refits before the rota builds
        /// it whole again. Sixty-four is about a second of a walking crowd at the frame rates this
        /// runs at, and a crowd of that many bodies comes round in as many.
        static constexpr std::uint64_t sRebuildEvery = 64;

        /// How many structures the rota has built whole again since the scene was made, for the
        /// scene's report.
        std::uint64_t getRebuildCount() const { return mRebuildCount; }

    private:
        /// Reserves room for the scene's geometry and copies in the runs `meshes` names. Per mesh
        /// and not per scene, because that is what an arrival is: the blocks already hold
        /// everything else.
        void writeGeometry(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes);

        /// Fills the refit build infos over the scratch `sizeRefitScratch` made. Leaves `mRefit`
        /// holding exactly this frame's rebuilds, which is what both the caller and `recordRefit`
        /// read.
        void prepareRefit(const SceneDesc& scene, FrameSlot slot);

        /// Grows the refit scratch to what any placement's refit can ask of it: every standing
        /// structure built to be refitted updated at once, and the one the rota builds whole taking
        /// its build's scratch in place of its update's — the largest such step. Where meshes
        /// arrive, and never on a placement: which mesh the rota picks moves every placement, and a
        /// scratch sized to each one's exact total would be a buffer made on whichever frame outgrew
        /// it.
        void sizeRefitScratch();

        /// What `mesh`'s place in this placement's refit works in: a whole build's scratch for the
        /// one the rota picked, an update's for the rest.
        VkDeviceSize refitScratchOf(Index mesh) const
        {
            return mesh == mRebuilt ? mBottomLevel.getBuildScratch(mesh) : mBottomLevel.getUpdateScratch(mesh);
        }

        /// Brings the host rows up to what `changed` names, and to whatever the table grew by.
        void writeRows(std::span<const InstanceRecord> records, std::span<const Index> changed);

        /// Everything the top-level build needs before a command buffer exists: `slot`'s copy of the
        /// rows paid, the structure and its scratch made again where the count grew, and the build
        /// pointed at that copy. `writeRows` first, which is what leaves the copy owing anything.
        void prepareTopLevel(const SceneDesc& scene, FrameSlot slot);

        /// Writes one row from its record.
        void placeRow(Index slot, const InstanceRecord& record);

        /// Makes the top level for `slots` rows, over storage grown to hold it.
        void sizeTopLevel(std::uint32_t slots);

        void recordRefit(VkCommandBuffer commands, GpuTimer* timer);
        void recordTopLevel(VkCommandBuffer commands, GpuTimer* timer);

        /// Writes again every row placing a mesh whose structure the compaction moved. True where
        /// anything moved, which is also when there is a copy to record.
        bool placeCompacted(std::span<const InstanceRecord> records);

        const Device& mDevice;

        /// Every deforming mesh's vertices as the frame tracing them sees them: the bind pose on
        /// arrival, and afterwards what `SkinPass` writes every frame a body moves. Indexed by
        /// `MeshRange::mBindOffset`, so the table is as long as the bodies rather than the cell — a
        /// static mesh's vertices are a build input `BottomLevelStore::build` stages. Blocked, so a
        /// scene that grows keeps the poses it was already given: a pose is on the device and
        /// nowhere else. A hit gets its vertices out of the structure through position fetch and
        /// reads these for one thing only: where its triangle stood on the previous frame, out of
        /// the other copy — `GpuTables::mPreviousPoseBlocks`.
        SlotBlocks mPoses{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };

        /// How many placements this has prepared that posed something, which is the clock the
        /// rebuild rota reads: a placement that poses nothing refits nothing and is not a tick of
        /// it. And which posed mesh the last one built whole.
        std::uint64_t mPlacements = 0;
        std::uint64_t mRebuildCount = 0;
        Index mRebuilt = sNoIndex;

        BlockedBuffer mIndices{ Shaders::INDEX_BLOCK, sizeof(std::uint32_t) };

        Buffer mTopLevelStorage;

        /// The rows the top level is built from, one copy per frame in flight. A gap is an inactive
        /// row — a reference of nought — and not a row left out, because a row's index is the slot a
        /// hit reads back.
        SlotTable<VkAccelerationStructureInstanceKHR> mRowTable;

        /// Kept across frames and built into again, made anew only when the slot table grows
        /// past what it was sized for: destroyed and created every frame, it would ask the driver
        /// for a size and a handle to build the same structure it had just thrown away.
        AccelerationStructure mTopLevel;

        /// How many rows the top level was made for, which is what its build ranges over.
        std::uint32_t mTopLevelSlots = 0;

        /// Every mesh slot, for the whole-scene build the constructor does through the same path an
        /// arrival takes. Kept so that path allocates nothing per scene.
        std::vector<Index> mEveryMesh;

        BottomLevelStore mBottomLevel;

        /// Kept across frames rather than made per refit, and sized by `sizeRefitScratch`. Grown
        /// through the graveyard and never destroyed outright: a frame places twice at a crossing,
        /// and a buffer freed under a build in flight was a device lost on every crossing.
        Buffer mRefitScratch;

        /// Grows to the high-water mark and stays, as the storage beside it does: made per frame,
        /// the pair was `vkAllocateMemory` twice on every frame that moved.
        Buffer mTopLevelScratch;

        /// The top-level build, prepared before a command buffer exists and recorded into one after.
        /// Members rather than locals because `pGeometries` is a pointer the build info keeps.
        VkAccelerationStructureGeometryKHR mTopLevelGeometry{};
        VkAccelerationStructureBuildGeometryInfoKHR mTopLevelBuild{};

        /// What one run of `prepareRefit` describes.
        StructureBuildBatch mRefit;

        /// The deformed meshes a refit rebuilds: every one the scene posed that has a structure,
        /// because one the device had no room for is left out. Refilled per placement.
        std::vector<Index> mRefitting;

        /// Two totals, each assigned, because one accumulated. The bottom levels are made once
        /// and the top level again every frame that moves, so adding both to one figure reported a
        /// scene that grew by its own top level sixty times a second.
        VkDeviceSize mTopLevelBytes = 0;
    };
}
