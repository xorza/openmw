#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/shaders/scene.h>

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
    class Graveyard;
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
        ~SceneAcceleration();

        /// Builds every mesh's structure, writes every row, and builds the top level, in one submit
        /// with each stage ending in the barrier the next one needs. Once, after the constructor.
        /// `scene` must place at least one instance: a top-level structure over nothing has no
        /// instance buffer to be built from.
        void build(Batch& batch, const SceneDesc& scene, std::span<const InstanceRecord> records, Graveyard& graveyard);

        /// Rebuilds what a moved world changed: every deformed mesh's structure, then the top level,
        /// in one command buffer with a barrier between — two `submitAndWait`s were a round trip
        /// through the driver in the middle of the frame for a dependency a barrier expresses.
        ///
        /// A deforming mesh's structure is refitted, not rebuilt: it was built with
        /// `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR`, which costs that mesh alone a
        /// larger structure, over the vertices `SkinPass` wrote into `slot`'s copy ahead of this.
        /// The whole of it is skipped where nothing moved and nothing deformed. Recorded into
        /// `placing.mCommands` and not submitted, so the caller decides whether the queue is asked
        /// now or with the frame; true where anything was recorded.
        ///
        /// @param changed the slots `updateInstanceRecords` wrote, which is the one list any of
        ///        this is driven by. `records` is handed in rather than made here because
        ///        `SceneBuffers` needs the same rows.
        bool place(const SceneDesc& scene, std::span<const InstanceRecord> records, std::span<const Index> changed,
            const Placing& placing);

        /// Takes in the geometry of the meshes the scene says arrived and lets go of the ones it says
        /// went. `buildArrived` builds their structures, once the pass has posed them. Every
        /// structure already built stays where it is, and the top level picks the change up for
        /// nothing. Safe with frames in flight, because nothing it writes is room one of them
        /// holds; `CI/check_rtx_validation.sh` is what says so.
        void extend(Batch& batch, const SceneDesc& scene, Graveyard& graveyard);

        /// Builds the structures of the meshes that arrived, over the first copy of the positions
        /// as `extend` and the pass left it.
        ///
        /// @param timer the frame the arrival lands in, so its builds are one zone of that frame's
        ///        report rather than device time nothing accounts for. Null for a picture inside the
        ///        interface, which is not timed — `VulkanRenderer::placeScene` says why.
        void buildArrived(Batch& batch, const SceneDesc& scene, GpuTimer* timer, Graveyard& graveyard);

        /// Destroys the structures of `meshes` and gives their storage back.
        void release(std::span<const Index> meshes, Graveyard& graveyard) { mBottomLevel.release(meshes, graveyard); }

        VkAccelerationStructureKHR getTopLevel() const { return mTopLevel; }

        /// Where the index blocks are, as a shader reads them at a hit. Here rather than in
        /// `SceneBuffers` because the build had to have them first. The address of a table of
        /// addresses, because the indices are a list of blocks a shader resolves
        /// `block[id / INDEX_BLOCK]` in itself.
        VkDeviceAddress getIndexBlocks() const { return mIndices.getTableAddress(); }

        /// The poses, for the pass that writes a deforming mesh's vertices into a slot's copy of
        /// them — and their account, which is what tells that pass which meshes each copy owes.
        SlotBlocks& getPoses() { return mPoses; }

        /// What the rows count as, kept by the row that changed rather than recounted over the
        /// table. `SceneStats` reports this record itself.
        const InstanceCounts& getInstanceCounts() const { return mCounts; }

        /// The room the structures were given, and what they occupy in it — a pair, because a
        /// structure copied tight gives its loose room back and a block is returned to the device
        /// only when nothing is left in it. Neither counts the geometry they were built from.
        VkDeviceSize getStructureBytes() const { return mBottomLevel.getBytes() + mTopLevelBytes; }
        VkDeviceSize getStructureLiveBytes() const { return mBottomLevel.getLiveBytes() + mTopLevelBytes; }

        VkDeviceSize getCompactableBytes() const { return mBottomLevel.getCompactableBytes(); }
        VkDeviceSize getCompactableNowBytes() const { return mBottomLevel.getCompactableNowBytes(); }

    private:
        /// Reserves room for the scene's geometry and copies in the runs `meshes` names.
        ///
        /// **Per mesh and not per scene**, because that is what an arrival is: the blocks already
        /// hold everything else, and rewriting them would be rewriting what nothing changed.
        void writeGeometry(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes);

        /// Fills the refit build infos and sizes the scratch. Leaves `mRefitBuilds` holding exactly
        /// this frame's rebuilds, which is what both the caller and `recordRefit` read.
        void prepareRefit(const SceneDesc& scene, FrameSlot slot, Graveyard& graveyard);

        /// Brings the host rows up to what `changed` names, and to whatever the table grew by.
        void writeRows(std::span<const InstanceRecord> records, std::span<const Index> changed);

        /// Everything the top-level build needs before a command buffer exists: `slot`'s copy of the
        /// rows paid, the structure and its scratch made again where the count grew, and the build
        /// pointed at that copy. `writeRows` first, which is what leaves the copy owing anything.
        void prepareTopLevel(const SceneDesc& scene, FrameSlot slot, Graveyard& graveyard);

        /// Takes back what `slot`'s row counts as, and leaves the row counting as nothing. The
        /// counts are kept by the row that changed rather than recounted over the table.
        void discountRow(Index slot);

        /// Writes one row from its record, keeping the counts in step.
        void placeRow(Index slot, const InstanceRecord& record);

        /// Makes the top level for `slots` rows, over storage grown to hold it.
        void sizeTopLevel(std::uint32_t slots, Graveyard& graveyard);

        void recordRefit(VkCommandBuffer commands, GpuTimer* timer);
        void recordTopLevel(VkCommandBuffer commands, GpuTimer* timer);

        /// Writes again every row placing a mesh whose structure the compaction moved. True where
        /// anything moved, which is also when there is a copy to record.
        bool placeCompacted(std::span<const InstanceRecord> records, Graveyard& graveyard);

        const Device& mDevice;

        /// Every deforming mesh's vertices as the frame tracing them sees them: the bind pose on
        /// arrival, and afterwards what `SkinPass` writes every frame a body moves. Indexed by
        /// `MeshRange::mBindOffset`, so the table is as long as the bodies rather than the cell — a
        /// static mesh's vertices are a build input `BottomLevelStore::build` stages. Blocked, so a
        /// scene that grows keeps the poses it was already given: a pose is on the device and
        /// nowhere else. Nothing reads these at a hit, which gets its vertices out of the structure
        /// through position fetch.
        SlotBlocks mPoses{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };
        std::uint32_t mSlots = 1;

        BlockedBuffer mIndices{ Shaders::INDEX_BLOCK, sizeof(std::uint32_t) };

        Buffer mTopLevelStorage;

        /// The rows the top level is built from, and one copy of them per frame in flight.
        ///
        /// A gap is an inactive row — a reference of nought — and not a row left out, because a
        /// row's index is the slot a hit reads back. `SlotTable` is what keeps the copies level.
        SlotTable<VkAccelerationStructureInstanceKHR> mRowTable;

        /// Kept across frames and built into again, made anew only when the slot table grows
        /// past what it was sized for: destroyed and created every frame, it would ask the driver
        /// for a size and a handle to build the same structure it had just thrown away.
        VkAccelerationStructureKHR mTopLevel = VK_NULL_HANDLE;

        /// How many rows the top level was made for, which is what its build ranges over.
        std::uint32_t mTopLevelSlots = 0;

        /// Every mesh slot, for the whole-scene build the constructor does through the same path an
        /// arrival takes. Kept so that path allocates nothing per scene.
        std::vector<Index> mEveryMesh;

        BottomLevelStore mBottomLevel;

        /// Kept across frames rather than made per refit, settling at the high-water mark. Grown
        /// through the graveyard and never destroyed outright: a frame places twice at a crossing,
        /// and a buffer freed under a build in flight was a device lost on every crossing.
        Buffer mRefitScratch;

        /// The top level's build scratch. This and the storage buffer beside it were
        /// `vkAllocateMemory` twice on every frame that moved; both grow to the high-water mark and
        /// stay.
        Buffer mTopLevelScratch;

        /// The top-level build, prepared before a command buffer exists and recorded into one after.
        /// Members rather than locals because `pGeometries` is a pointer the build info keeps.
        VkAccelerationStructureGeometryKHR mTopLevelGeometry{};
        VkAccelerationStructureBuildGeometryInfoKHR mTopLevelBuild{};

        /// What one run of `prepareRefit` describes.
        StructureBuildBatch mRefit;

        /// What each row counts as — `sRowCutout`, `sRowWater` — so the counts below can be kept by
        /// the row that changed rather than recounted over every row a frame.
        std::vector<std::uint8_t> mRowFlags;

        InstanceCounts mCounts;

        /// **Two totals, each assigned, because one accumulated.** The bottom levels are made once
        /// and the top level again every frame that moves, so adding both to one figure reported a
        /// scene that grew by its own top level sixty times a second.
        VkDeviceSize mTopLevelBytes = 0;
    };
}
