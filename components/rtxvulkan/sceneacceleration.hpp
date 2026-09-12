#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/instancecounts.hpp>
#include <components/rtx/instancerecord.hpp>
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
    struct SceneTables;

    /// The neutral transform in Vulkan's storage.
    ///
    /// `VkTransformMatrixKHR` is three rows of four, which is exactly what `Transform3x4` holds, so
    /// this restates the rows and changes nothing. The transposition that matters happened in
    /// `toTransform3x4`, once, where a backend cannot get it wrong on its own.
    VkTransformMatrixKHR toVulkanTransform(const Transform3x4& transform);

    /// Every acceleration structure a scene needs, built once.
    ///
    /// The top level over the instances, the refit that keeps a deforming mesh's structure over its
    /// pose, and the rows both are built from. The bottom levels themselves are `BottomLevelStore`'s,
    /// which is where their compaction is too.
    class SceneAcceleration
    {
    public:
        /// Writes every mesh's geometry and every row, and builds nothing yet.
        ///
        /// **The build is a second step, because a pose comes between.** A skinned body's structure
        /// is built over the vertices in the first copy of the positions, and what is there after
        /// this constructor is the bind pose the mesh arrived with — a body in skin space, which is
        /// not where the body is. `SkinPass` writes the pose into that copy, and `build` then builds
        /// over what the frame will trace; a structure built over the bind and refitted into the
        /// pose would keep the bind's shape for the life of the mesh.
        ///
        /// @param slots how many frames may be tracing this scene at once, which is how many copies
        ///        there are of the rows and of the positions a refit reads.
        SceneAcceleration(const Device& device, Batch& batch, const SceneTables& scene, std::uint32_t slots);
        ~SceneAcceleration();

        SceneAcceleration(const SceneAcceleration&) = delete;
        SceneAcceleration& operator=(const SceneAcceleration&) = delete;

        /// Builds every mesh's structure, writes every row, and builds the top level. Once, after
        /// the constructor.
        ///
        /// **The geometry, every bottom level and the top level in one submit.** Each was its own
        /// round trip; the host writes are visible to the submit without a barrier, and each stage
        /// ends in the barrier the next one needs.
        ///
        /// `scene` must place at least one instance: a top-level structure over nothing has no
        /// instance buffer to be built from. `records` are `scene`'s rows, made by the caller for
        /// the reason `place` gives.
        void build(
            Batch& batch, const SceneTables& scene, std::span<const InstanceRecord> records, Graveyard& graveyard);

        /// Rebuilds what a moved world changed: every deformed mesh's structure, then the top level.
        ///
        /// **One submit for both, because the device is idle across a fence.** These were two
        /// `submitAndWait` calls, and the second could not begin recording until the first had
        /// finished on the queue — a round trip through the driver in the middle of the frame for a
        /// dependency a pipeline barrier already expresses. They go in one command buffer with that
        /// barrier between them.
        ///
        /// The deformed half is what a skinned body is: its triangles never change and its vertices
        /// change every frame, so the mesh keeps its slice of the shared position buffer and only
        /// the contents of that slice — and the structure over it — are made again. **Refitted, not
        /// rebuilt**: a deforming mesh's structure was built with
        /// `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR`, which costs that mesh alone a
        /// larger structure, and every static mesh in the cell keeps the tight one. The vertices the
        /// refit reads are what `SkinPass` wrote into `slot`'s copy ahead of this, in the same
        /// command buffer.
        ///
        /// **And the whole of it is skipped where nothing moved and nothing deformed**, which is
        /// every frame of a standing camera in a place with no actor: the top level is the same top
        /// level, and there is nothing to submit.
        ///
        /// **`records` is handed in rather than made here**, because `SceneBuffers` needs the same
        /// rows and building them twice was thousands of matrix inversions a frame done again for
        /// the same answer. `scene` must name the same meshes in the same order — the instances
        /// index into the structures this already holds.
        ///
        /// **Recorded into `placing.mCommands` and not submitted**, so the caller decides whether
        /// the queue is asked now or with the frame. Into that placement's copy of the rows, which
        /// the caller has made sure no frame is still reading. True where anything was recorded; a
        /// frame in which nothing moved and nothing deformed records nothing and needs no submit.
        ///
        /// @param changed the slots `updateInstanceRecords` wrote, which is the one list any of
        ///        this is driven by. Whether a copy is then behind is `mRowTable`'s to know.
        bool place(const SceneTables& scene, std::span<const InstanceRecord> records, std::span<const Index> changed,
            const Placing& placing);

        /// Takes in the geometry of the meshes the scene says arrived and lets go of the ones it says
        /// went. `buildArrived` builds their structures, once the pass has posed them.
        ///
        /// **What a cell crossing costs, instead of the world.** Every structure already built stays
        /// where it is: the geometry blocks are appended to rather than replaced, so the addresses
        /// they were built from are still theirs, and the storage a departing mesh gives back is
        /// handed to the next one that fits. The top level is rebuilt every frame regardless and
        /// picks the change up for nothing.
        ///
        /// **Safe with frames in flight**, because nothing it writes is room one of them holds: a
        /// block is only appended to, a mesh's run is one no placed instance names, and what an
        /// arrival replaces goes to `graveyard`. `CI/check_rtx_validation.sh` is what says so — a
        /// route of nineteen crossings under synchronization validation, with the ring undrained.
        void extend(Batch& batch, const SceneTables& scene, Graveyard& graveyard);

        /// Builds the structures of the meshes that arrived, over the first copy of the positions
        /// as `extend` and the pass left it.
        ///
        /// @param timer the frame the arrival lands in, so its builds are one zone of that frame's
        ///        report rather than device time nothing accounts for. Null for a picture inside the
        ///        interface, which is not timed — `VulkanRenderer::placeScene` says why.
        void buildArrived(Batch& batch, const SceneTables& scene, GpuTimer* timer, Graveyard& graveyard);

        /// Destroys the structures of `meshes` and gives their storage back.
        void release(std::span<const Index> meshes, Graveyard& graveyard) { mBottomLevel.release(meshes, graveyard); }

        VkAccelerationStructureKHR getTopLevel() const { return mTopLevel; }

        /// Where the index blocks are, as a shader reads them.
        ///
        /// A shader needs the same indices at a hit, to find which three vertices it landed between.
        /// They are here rather than in `SceneBuffers` because the build had to have them first, and
        /// uploading a cell's worth of them twice is a megabyte for nothing.
        ///
        /// **The address of a table of addresses, and not the data**, because the indices are a list
        /// of blocks: what the frame carries is where the blocks are, and a shader resolves
        /// `block[id / INDEX_BLOCK]` itself.
        VkDeviceAddress getIndexBlocks() const { return mIndices.getTableAddress(); }

        /// The poses, for the pass that writes a deforming mesh's vertices into a slot's copy of
        /// them — and their account, which is what tells that pass which meshes each copy owes.
        SlotBlocks& getPoses() { return mPoses; }

        /// What the rows count as, kept by the row that changed rather than recounted over the
        /// table. `SceneStats` reports this record itself.
        const InstanceCounts& getInstanceCounts() const { return mCounts; }

        /// The room the structures were given, and what they occupy in it. Neither counts the
        /// geometry they were built from.
        ///
        /// **A pair, because compaction moves the two apart.** A structure copied tight gives its
        /// loose room back, and a block is returned to the device only when nothing is left in it —
        /// so the reservation says what a cell asked for and the live figure says what it kept.
        VkDeviceSize getStructureBytes() const { return mBottomLevel.getBytes() + mTopLevelBytes; }
        VkDeviceSize getStructureLiveBytes() const { return mBottomLevel.getLiveBytes() + mTopLevelBytes; }

        VkDeviceSize getCompactableBytes() const { return mBottomLevel.getCompactableBytes(); }
        VkDeviceSize getCompactableNowBytes() const { return mBottomLevel.getCompactableNowBytes(); }

    private:
        /// Reserves room for the scene's geometry and copies in the runs `meshes` names.
        ///
        /// **Per mesh and not per scene**, because that is what an arrival is: the blocks already
        /// hold everything else, and rewriting them would be rewriting what nothing changed.
        void writeGeometry(Batch& batch, const SceneTables& scene, std::span<const Index> meshes);

        /// Fills the refit build infos and sizes the scratch.
        ///
        /// Leaves `mRefitBuilds` holding exactly this frame's rebuilds and nothing else, which is
        /// what both the caller and `recordRefit` read: a count returned beside a vector that still
        /// held the last frame's entries would be two answers to one question.
        void prepareRefit(const SceneTables& scene, FrameSlot slot, Graveyard& graveyard);

        /// Brings the host rows up to what `changed` names, and to whatever the table grew by.
        void writeRows(std::span<const InstanceRecord> records, std::span<const Index> changed);

        /// Everything the top-level build needs before a command buffer exists: `slot`'s copy of the
        /// rows paid, the structure and its scratch made again where the count grew, and the build
        /// pointed at that copy. `writeRows` first, which is what leaves the copy owing anything.
        void prepareTopLevel(const SceneTables& scene, FrameSlot slot, Graveyard& graveyard);

        /// Takes back what `slot`'s row counts as, and leaves the row counting as nothing.
        ///
        /// **What a row leaving owes.** The counts are kept by the row that changed rather than
        /// recounted over the table, so a row overwritten and a row dropped both have to discount
        /// themselves — and a drop has no record to write afterwards.
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
        /// arrival, and afterwards what `SkinPass` writes for a body every frame it moves — into
        /// the copy the refit reads, in the same command buffer, with a barrier between.
        ///
        /// **Indexed by `MeshRange::mBindOffset`, so the table is as long as the bodies rather than
        /// as long as the cell.** A static mesh has no run here at all: its vertices are a build
        /// input that does not outlive the build, and `BottomLevelStore::build` stages it. A cell
        /// deforms a twentieth of its drawables, and a copy of this reserved for all of them would
        /// be twenty times the size.
        ///
        /// **Blocked, so a scene that grows keeps the poses it was already given.** A pose is on the
        /// device and nowhere else — the host holds a bind pose and a set of bone rows — so a table
        /// remade would show every standing body its bind pose until something moved it again.
        ///
        /// Nothing reads these at a hit: a hit gets its vertices back out of the structure through
        /// position fetch, so they are a build input and a pose's destination and nothing else,
        /// which is why there is no table of their addresses beside them.
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

        /// Kept across frames rather than made per refit: a device allocation on the frame path is a
        /// stall, and this settles at the high-water mark of whatever the world is showing. It never
        /// shrinks, which is what makes it settle at all.
        ///
        /// **Grown through the graveyard and never destroyed outright.** A placement's submit
        /// carries no fence of its own, and a frame places twice at a crossing: the second
        /// placement's refits can want more scratch while the first's are still on the queue, and
        /// a buffer freed under a build in flight was a device lost on every crossing — measured,
        /// once an arrival was slow enough to leave the first placement on the queue to be caught.
        Buffer mRefitScratch;

        /// The top level's build scratch, which was made and freed on every frame that moved.
        ///
        /// **`vkAllocateMemory` twice on every frame that moves** — this and the storage buffer
        /// beside it — where the driver's allocator is exactly the thing a frame budget cannot see
        /// into. Both grow to the high-water mark and stay.
        Buffer mTopLevelScratch;

        /// The top-level build, prepared before a command buffer exists and recorded into one after.
        ///
        /// Members rather than locals because `pGeometries` is a pointer the build info keeps: the
        /// geometry has to outlive the preparation that named it. The build range does not — it is
        /// the placed count and nothing else, so `recordTopLevel` makes its own.
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
