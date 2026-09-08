#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/slotset.hpp>

#include "blockedbuffer.hpp"
#include "buffer.hpp"
#include "frameslots.hpp"
#include "owned.hpp"
#include "placing.hpp"
#include "slottable.hpp"
#include "structurestorage.hpp"

namespace Rtx
{
    class Batch;
    class CommandPool;
    class GpuTimer;
    class Device;
    class Graveyard;
    class SceneDesc;
    class SceneMicromaps;

    /// The neutral transform in Vulkan's storage.
    ///
    /// `VkTransformMatrixKHR` is three rows of four, which is exactly what `Transform3x4` holds, so
    /// this restates the rows and changes nothing. The transposition that matters happened in
    /// `toTransform3x4`, once, where a backend cannot get it wrong on its own.
    VkTransformMatrixKHR toVulkanTransform(const Transform3x4& transform);

    /// The scratch one run of structure builds is described in.
    ///
    /// **Members and not locals, because Vulkan keeps the addresses.** A build info holds
    /// `pGeometries` as a pointer and a range is handed over by address, so both have to outlive the
    /// loop that filled them — and a cell arriving must not allocate five vectors to say so.
    ///
    /// **Two passes, because `sizeTo` is what makes the first one possible.** A vector grown while a
    /// pointer already points into it moves its storage, so every geometry is placed before any
    /// build info names one. `buildMeshes` and `prepareRefit` are each written that way, and this is
    /// where the rule is stated rather than in both of them.
    struct StructureBuildBatch
    {
        std::vector<VkAccelerationStructureGeometryKHR> mGeometries;

        /// What each geometry chains for its micromap, where it has one. Beside the geometries
        /// because the geometry keeps a pointer to it.
        std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> mMicromaps;

        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mBuilds;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> mRanges;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> mRangePointers;

        /// Room for `count` descriptions, each one cleared, and an empty list of range pointers.
        ///
        /// **Cleared and not merely sized**, because a filler may skip an entry — a mesh with no
        /// triangles is described by nobody — and what is left behind is then the previous run's.
        ///
        /// The pointers are pushed rather than sized, because how many there are is what a filler
        /// decides: every entry for a refit, and only what was really built otherwise.
        void sizeTo(std::size_t count)
        {
            mGeometries.assign(count, VkAccelerationStructureGeometryKHR{});
            mMicromaps.assign(count, VkAccelerationStructureTrianglesOpacityMicromapEXT{});
            mBuilds.assign(count, VkAccelerationStructureBuildGeometryInfoKHR{});
            mRanges.assign(count, VkAccelerationStructureBuildRangeInfoKHR{});

            mRangePointers.clear();
            mRangePointers.reserve(count);
        }
    };

    /// Every acceleration structure a scene needs, built once.
    ///
    /// One bottom-level structure per mesh, all of them inside a single buffer at offsets, and one
    /// top-level structure over the instances. Per-mesh buffers would be the obvious shape and would
    /// spend a device allocation on each of a cell's several hundred meshes; the scene description is
    /// flat for the same reason.
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
        /// @param slots how many frames may be tracing this scene at once — `sFrameSlots` for the
        ///        world, one for a picture inside the interface — which is how many copies there are
        ///        of the rows and of the positions a refit reads.
        SceneAcceleration(const Device& device, Batch& batch, const SceneDesc& scene, std::uint32_t slots);
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
        /// the reason `place` gives. `micromaps` has baked whatever the scene's cutouts take, and
        /// each structure is built over its mesh's — `SceneMicromaps::describe` says what a build
        /// chains and why a refit chains the same.
        void build(Batch& batch, const SceneDesc& scene, std::span<const InstanceRecord> records,
            const SceneMicromaps& micromaps, Graveyard& graveyard);

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
        /// @param micromaps what each refitted mesh's structure was built over, which an update has
        ///        to describe again.
        bool place(const SceneDesc& scene, std::span<const InstanceRecord> records, std::span<const Index> changed,
            const SceneMicromaps& micromaps, const Placing& placing);

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
        void extend(Batch& batch, const SceneDesc& scene, Graveyard& graveyard);

        /// Builds the structures of the meshes that arrived, over the first copy of the positions
        /// as `extend` and the pass left it.
        ///
        /// @param timer the frame the arrival lands in, so its builds are one zone of that frame's
        ///        report rather than device time nothing accounts for. Null for a picture inside the
        ///        interface, which is not timed — `VulkanRenderer::placeScene` says why.
        void buildArrived(Batch& batch, const SceneDesc& scene, const SceneMicromaps& micromaps, GpuTimer* timer,
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

        /// Where `mesh`'s indices start, for the bake that reads a cutout's triangles through
        /// them. One address covers the run: a mesh never straddles a block.
        VkDeviceAddress getIndices(const MeshRange& mesh) const { return mIndices.addressOf(mesh.mIndices.mOffset); }

        /// Every mesh slot the constructor was handed, which is what a whole-scene bake and build
        /// walk.
        std::span<const Index> getEveryMesh() const { return mEveryMesh; }

        /// The poses, for the pass that writes a deforming mesh's vertices into a slot's copy of
        /// them — and their account, which is what tells that pass which meshes each copy owes.
        SlotBlocks& getPoses() { return mPoses; }

        std::uint32_t getInstanceCount() const { return mInstanceCount; }

        /// How many of those instances traversal has to stop and ask about.
        ///
        /// The cost of the cutout, as a number: every one of these is a candidate loop and a texture
        /// fetch where an opaque instance is a hit. Reported so that a material change that marks
        /// half a cell non-opaque shows up as a number before it shows up as a frame time.
        std::uint32_t getCutoutInstanceCount() const { return mCutoutInstanceCount; }

        /// How many of the cutouts place a mesh whose structure carries an opacity micromap, and
        /// are not being faded — `placeRow` says why a fade reads its leaves through the any-hit.
        std::uint32_t getMicromappedInstanceCount() const { return mMicromappedInstanceCount; }

        /// How many of them the eye meets as water.
        ///
        /// **What says whether a trace needs the sea at all.** A frame's water level says where a
        /// surface would be and not whether there is one, and a room with neither is a kernel with
        /// no waves, no caustics and no underwater column in it — `HAS_SEA` is what removes them.
        std::uint32_t getWaterInstanceCount() const { return mWaterInstanceCount; }

        /// How many of them are a medium the eye passes through — `Rtx::Material::isMedium`.
        ///
        /// **What says whether the trace has to gather one at all.** `mediumAlong` walks the
        /// structure on a mask of its own, and where no instance carries that mask the walk still
        /// descends the top level and finds nothing: measured at 0.02 ms of a 1.86 ms trace over
        /// Seyda Neen. `VisibilityConstants::mMediumInFrame` is what carries this to the shader.
        std::uint32_t getMediumInstanceCount() const { return mMediumInstanceCount; }

        /// The room the structures were given, and what they occupy in it. Neither counts the
        /// geometry they were built from.
        ///
        /// **A pair, because compaction moves the two apart.** A structure copied tight gives its
        /// loose room back, and a block is returned to the device only when nothing is left in it —
        /// so the reservation says what a cell asked for and the live figure says what it kept.
        VkDeviceSize getStructureBytes() const { return mBottomLevelStorage.getBytes() + mTopLevelBytes; }
        VkDeviceSize getStructureLiveBytes() const { return mBottomLevelStorage.getLiveBytes() + mTopLevelBytes; }

        /// What the structures still to be copied tight would come to, or nought where there are
        /// none and where the device would not say.
        ///
        /// **What is left to save, and so nought once a cell has settled.** A structure is built
        /// loose because the builder cannot know the answer until it has finished, and `place`
        /// copies each into the size it turned out to need at a budget per placement. So this falls
        /// to nothing over the placements after an arrival, while `getStructureBytes` falls by what
        /// it named.
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

        /// Reserves room for the scene's geometry and copies in the runs `meshes` names.
        ///
        /// **Per mesh and not per scene**, because that is what an arrival is: the blocks already
        /// hold everything else, and rewriting them would be rewriting what nothing changed.
        void writeGeometry(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes);

        /// Creates and records the build of a structure for each of `meshes`, taking storage for it.
        ///
        /// A slot that already holds one has it destroyed and its room given back first: a slot the
        /// scene took back and handed out again arrives carrying different geometry.
        void buildMeshes(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes,
            const SceneMicromaps& micromaps, Graveyard& graveyard);

        /// Fills the refit build infos and sizes the scratch.
        ///
        /// Leaves `mRefitBuilds` holding exactly this frame's rebuilds and nothing else, which is
        /// what both the caller and `recordRefit` read: a count returned beside a vector that still
        /// held the last frame's entries would be two answers to one question.
        void prepareRefit(
            const SceneDesc& scene, std::uint32_t slot, const SceneMicromaps& micromaps, Graveyard& graveyard);

        /// Brings the host rows up to what `changed` names, and to whatever the table grew by.
        void writeRows(std::span<const InstanceRecord> records, std::span<const Index> changed);

        /// Everything the top-level build needs before a command buffer exists: `slot`'s copy of the
        /// rows paid, the structure and its scratch made again where the count grew, and the build
        /// pointed at that copy. `writeRows` first, which is what leaves the copy owing anything.
        void prepareTopLevel(const SceneDesc& scene, std::uint32_t slot, Graveyard& graveyard);

        /// Writes one row from its record, keeping the counts in step.
        void placeRow(Index slot, const InstanceRecord& record);

        /// Makes the top level for `slots` rows, over storage grown to hold it.
        void sizeTopLevel(std::uint32_t slots, Graveyard& graveyard);

        void recordRefit(VkCommandBuffer commands, GpuTimer* timer);
        void recordTopLevel(VkCommandBuffer commands, GpuTimer* timer);

        /// Writes what a tight copy of each structure the last build made would come to.
        void askWhatCompactionWouldSave(VkCommandBuffer commands);

        /// Reads those answers, once the placement that asked for them has certainly run, and makes
        /// a tight structure for as many as this placement's budget takes. Every row that placed one
        /// is written again, because the address it named has moved. True where anything is left for
        /// `recordCompaction` to copy.
        bool prepareCompaction(std::span<const InstanceRecord> records, Graveyard& graveyard);

        /// Copies each structure `prepareCompaction` made room for into it.
        void recordCompaction(VkCommandBuffer commands, GpuTimer* timer);

        const Device& mDevice;

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

        /// The meshes those copies moved, for the walk that writes the rows placing them again.
        SlotSet mMovedMeshes;

        /// How many placements this scene has been through, and which one asked the compaction
        /// questions — `sNoPlacement` where none are outstanding.
        ///
        /// **What stands in for a fence.** The ring waits for the frame `mSlots` back before it
        /// records this one, so a placement that far behind has finished on the queue and its
        /// answers are there to be read. Asking with `WAIT_BIT` instead would stall the frame a
        /// cell arrives in, which is the one frame that can least afford it.
        std::uint64_t mPlacements = 0;
        std::uint64_t mQueriedAt = sNoPlacement;

        /// Every deforming mesh's vertices as the frame tracing them sees them: the bind pose on
        /// arrival, and afterwards what `SkinPass` writes for a body every frame it moves — into
        /// the copy the refit reads, in the same command buffer, with a barrier between.
        ///
        /// **Indexed by `MeshRange::mBindOffset`, so the table is as long as the bodies rather than
        /// as long as the cell.** A static mesh has no run here at all: its vertices are a build
        /// input that does not outlive the build, and `buildMeshes` is where they are staged. Seyda
        /// Neen is 138 deforming drawables of 2800, and every copy of this used to be reserved for
        /// all of them.
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

        StructureStorage mBottomLevelStorage{ VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            "bottom level structures" };

        Buffer mTopLevelStorage;

        /// The rows the top level is built from, and one copy of them per frame in flight.
        ///
        /// A gap is an inactive row — a reference of nought — and not a row left out, because a
        /// row's index is the slot a hit reads back. `SlotTable` is what keeps the copies level.
        SlotTable<VkAccelerationStructureInstanceKHR> mRowTable;

        std::vector<VkAccelerationStructureKHR> mBottomLevel;

        /// Where each of those sits in the storage, so a released mesh can give its room back.
        std::vector<StructureRoom> mBottomLevelRooms;

        /// Each of those structures' device address, asked for once when it was made.
        ///
        /// **Not once per instance per frame, which is what this replaced.** A handle lasts from one
        /// `setScene` to the next and its address with it, so a nine-by-nine exterior was making
        /// fifty thousand driver calls a frame to be told the same fifty thousand numbers.
        std::vector<VkDeviceAddress> mBottomLevelAddresses;

        /// Kept across frames and built into again, made anew only when the slot table grows
        /// past what it was sized for. It was destroyed and created every frame, which asked the
        /// driver for a size and a handle to build the same structure it had just thrown away.
        VkAccelerationStructureKHR mTopLevel = VK_NULL_HANDLE;

        /// How many rows the top level was made for, which is what its build ranges over.
        std::uint32_t mTopLevelSlots = 0;

        /// What a refit of each mesh asks for, so a frame does not have to ask the driver again.
        /// Nought for a mesh that was not built to be refitted.
        std::vector<VkDeviceSize> mUpdateScratch;

        /// Whether each mesh's structure was built with `ALLOW_UPDATE`, which is whether the scene's
        /// `MeshRange::mDeform` named a kind at the time it was built. A mesh's kind is fixed when
        /// it arrives, so this is also whether the mesh can ever be in `getDeformed`.
        std::vector<std::uint8_t> mUpdatable;

        /// Whether each mesh's structure was built over a micromap, which is what a row placing it
        /// counts by and what a refit of it has to describe again.
        std::vector<std::uint8_t> mMicromapped;

        /// Every mesh slot, for the whole-scene build the constructor does through the same path an
        /// arrival takes. Kept so that path allocates nothing per scene.
        std::vector<Index> mEveryMesh;

        /// What one run of `buildMeshes` describes.
        StructureBuildBatch mBuild;

        /// How big each mesh's structure comes out, and where in the one scratch buffer they share
        /// its build takes its working room. Beside each other because both are filled in the same
        /// pass and read in the next.
        std::vector<VkDeviceSize> mBuildSizes;
        std::vector<VkDeviceSize> mBuildScratchOffsets;

        /// Where each arriving static mesh's vertices sit in the buffer `buildMeshes` stages them
        /// into, in bytes. Meaningless for a mesh that deforms, which is built from its pose.
        std::vector<VkDeviceSize> mArrivedAt;

        /// The builds actually recorded, which is `mBuild.mBuilds` without the meshes that came out
        /// at nought bytes — a mesh with no triangles is described by nobody and built by nobody.
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mLiveBuilds;

        /// Kept across frames rather than made per refit: a device allocation on the frame path is a
        /// stall, and this settles at the high-water mark of whatever the world is showing. It never
        /// shrinks, which is what makes it settle at all.
        ///
        /// **Grown through the graveyard and never destroyed outright.** A placement's submit
        /// carries no fence of its own, and a frame places twice at a crossing: the second
        /// placement's refits can want more scratch while the first's are still on the queue, and
        /// a buffer freed under a build in flight was a device lost on every crossing — measured,
        /// once an arrival's bake left the first placement on the queue long enough to be caught.
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
        /// `mInstanceCount` and nothing else, so `recordTopLevel` makes its own.
        VkAccelerationStructureGeometryKHR mTopLevelGeometry{};
        VkAccelerationStructureBuildGeometryInfoKHR mTopLevelBuild{};

        /// What one run of `prepareRefit` describes.
        StructureBuildBatch mRefit;

        /// What each row counts as — `sRowCutout`, `sRowWater` — so the counts below can be kept by
        /// the row that changed rather than recounted over every row a frame.
        std::vector<std::uint8_t> mRowFlags;

        std::uint32_t mInstanceCount = 0;
        std::uint32_t mCutoutInstanceCount = 0;
        std::uint32_t mMicromappedInstanceCount = 0;
        std::uint32_t mWaterInstanceCount = 0;
        std::uint32_t mMediumInstanceCount = 0;

        /// **Two totals, each assigned, because one accumulated.** The bottom levels are made once
        /// and the top level again every frame that moves, so adding both to one figure reported a
        /// scene that grew by its own top level sixty times a second.
        VkDeviceSize mTopLevelBytes = 0;
    };
}
