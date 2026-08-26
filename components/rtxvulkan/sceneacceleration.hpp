#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <osg/Vec3f>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/micromap.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/texturedata.hpp>

#include "blockedbuffer.hpp"
#include "buffer.hpp"
#include "hostbuffer.hpp"
#include "structurestorage.hpp"

namespace Rtx
{
    class Batch;
    class CommandPool;
    class GpuTimer;
    class Device;
    class SceneDesc;

    /// The neutral transform in Vulkan's storage.
    ///
    /// `VkTransformMatrixKHR` is three rows of four, which is exactly what `Transform3x4` holds, so
    /// this restates the rows and changes nothing. The transposition that matters happened in
    /// `toTransform3x4`, once, where a backend cannot get it wrong on its own.
    VkTransformMatrixKHR toVulkanTransform(const Transform3x4& transform);

    /// One mesh's micromap usage counts, kept because two commands are given them.
    ///
    /// The micromap build is told how many triangles it holds at each subdivision level, and so is
    /// the acceleration structure that then references it — and the second of those happens in a
    /// later call, so the counts cannot be a local of the first.
    ///
    /// A fixed array rather than a vector because there are at most `sSubdivisionCeiling + 1` of
    /// them and a scene is thousands of meshes: a vector apiece is an allocation apiece for
    /// seventy-odd bytes.
    struct MicromapUsageCounts
    {
        std::array<VkMicromapUsageEXT, Micromap::sSubdivisionCeiling + 1> mCounts{};
        std::uint32_t mCount = 0;
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
        /// `scene` must place at least one instance: a top-level structure over nothing has no
        /// instance buffer to be built from. `records` are `scene`'s rows, made by the caller for
        /// the reason `place` gives.
        /// @param textures every image the scene names, which the opacity micromaps are classified
        ///        against. A mesh whose cutout mask is not among them gets no micromap and goes on
        ///        asking, which is what the whole cell did before there were any.
        SceneAcceleration(const Device& device, Batch& batch, const SceneDesc& scene,
            std::span<const InstanceRecord> records, std::span<const TextureData> textures);
        ~SceneAcceleration();

        SceneAcceleration(const SceneAcceleration&) = delete;
        SceneAcceleration& operator=(const SceneAcceleration&) = delete;

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
        /// the contents of that slice — and the structure over it — are made again. A rebuild rather
        /// than a refit: `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR` costs every static
        /// mesh in the cell a larger structure and a slower trace to make a few dozen actors cheaper
        /// to animate, which is M12's measurement to take and not an assumption to build on. It is
        /// skipped outright where nothing deformed, which is every frame of a world with no actor.
        ///
        /// **`records` is handed in rather than made here**, because `SceneBuffers` needs the same
        /// rows and building them twice was thousands of matrix inversions a frame done again for
        /// the same answer. `scene` must name the same meshes in the same order — the instances
        /// index into the structures this already holds.
        void place(CommandPool& pool, const SceneDesc& scene, std::span<const InstanceRecord> records, GpuTimer* timer);

        /// Takes in the meshes the scene says arrived and lets go of the ones it says went.
        ///
        /// **What a cell crossing costs, instead of the world.** Every structure already built stays
        /// where it is: the geometry blocks are appended to rather than replaced, so the addresses
        /// they were built from are still theirs, and the storage a departing mesh gives back is
        /// handed to the next one that fits. The top level is rebuilt every frame regardless and
        /// picks the change up for nothing.
        void extend(Batch& batch, const SceneDesc& scene, std::span<const TextureData> textures);

        /// Destroys the structures of `meshes` and gives their storage back.
        ///
        /// **Idempotent**, because both the frame that places and the one that appends run it: a
        /// slot whose structure has already gone holds no handle and no room, and asking again is a
        /// pair of comparisons.
        void release(std::span<const Index> meshes);

        VkAccelerationStructureKHR getTopLevel() const { return mTopLevel; }

        /// Where the index blocks are, as a shader reads them.
        ///
        /// A shader needs the same indices at a hit, to find which three vertices it landed between.
        /// They are here rather than in `SceneBuffers` because the build had to have them first, and
        /// uploading a cell's worth of them twice is a megabyte for nothing.
        ///
        /// **A table of addresses and not the data**, because the indices are a list of blocks: what
        /// a shader binds is where the blocks are, and it resolves `block[id / INDEX_BLOCK]` itself.
        VkBuffer getIndexBlocks() const { return mIndices.getTable(); }
        std::uint32_t getInstanceCount() const { return mInstanceCount; }

        /// How many of those instances traversal has to stop and ask about.
        ///
        /// The cost of the cutout, as a number: every one of these is a candidate loop and a texture
        /// fetch where an opaque instance is a hit. Reported so that a material change that marks
        /// half a cell non-opaque shows up as a number before it shows up as a frame time.
        std::uint32_t getCutoutInstanceCount() const { return mCutoutInstanceCount; }

        /// How many of those a micromap answers for, so traversal asks the shader only where the
        /// mask actually straddles the cutoff.
        ///
        /// **The number that says the micromaps are doing anything**, and the one that goes down
        /// when a mesh's mask could not be classified. Counted in instances rather than meshes
        /// because that is what traversal meets.
        std::uint32_t getMicromappedInstanceCount() const { return mMicromappedInstanceCount; }

        /// How much of the micromapped geometry each verdict covers, in triangles.
        ///
        /// **This is what says a micromap is worth its memory, and no other number can.** A build
        /// that resolved nothing and one that was never consulted trace the same and draw the same
        /// frame; only the share of the surface that stopped asking tells them apart.
        MicromapTally getMicromapTally() const;

        /// Bytes held by the structures themselves, not counting the geometry they were built from.
        VkDeviceSize getStructureBytes() const { return mBottomLevelStorage.getBytes() + mTopLevelBytes; }

    private:
        /// Builds an opacity micromap for each of `meshes` whose cutout can be classified.
        ///
        /// **Before the structures, in the same recording**, because a bottom level that references
        /// a micromap is built from it: the two are separated by a barrier and not by a submit.
        ///
        /// A mesh qualifies only where every placement standing on it names the *same* cutout
        /// material. A micromap belongs to the structure and so to the mesh, while a cutout belongs
        /// to the material — so a mesh two materials disagree about has no one answer to give, and
        /// the honest reply is none at all.
        void buildMicromaps(
            Batch& batch, const SceneDesc& scene, std::span<const TextureData> textures, std::span<const Index> meshes);

        /// Destroys `mesh`'s micromap and gives its room back. Idempotent, like `release`.
        void releaseMicromap(Index mesh);

        /// Chains `mesh`'s micromap onto the geometry describing it, where it has one.
        ///
        /// **Both build paths run through this**, because a refit describes the same geometry the
        /// first build did: a pose moves vertices and a micromap is about texture coordinates and a
        /// mask, so a refit that dropped the chain would put the leaf and the hole back only while
        /// something was animating.
        ///
        /// `link` is written rather than returned because the geometry keeps it by address, so it
        /// has to outlive the call — the caller holds one per geometry for exactly that reason.
        void attachMicromap(Index mesh, VkAccelerationStructureGeometryKHR& geometry,
            VkAccelerationStructureTrianglesOpacityMicromapEXT& link) const;

        /// Reserves room for the scene's geometry and copies in the runs `meshes` names.
        ///
        /// **Per mesh and not per scene**, because that is what an arrival is: the blocks already
        /// hold everything else, and rewriting them would be rewriting what nothing changed.
        void writeGeometry(const SceneDesc& scene, std::span<const Index> meshes);

        /// Creates and records the build of a structure for each of `meshes`, taking storage for it.
        ///
        /// A slot that already holds one has it destroyed and its room given back first: a slot the
        /// scene took back and handed out again arrives carrying different geometry.
        void buildMeshes(Batch& batch, const SceneDesc& scene, std::span<const Index> meshes);

        /// Fills the refit build infos and sizes the scratch.
        ///
        /// Leaves `mRefitBuilds` holding exactly this frame's rebuilds and nothing else, which is
        /// what both the caller and `recordRefit` read: a count returned beside a vector that still
        /// held the last frame's entries would be two answers to one question.
        void prepareRefit(const SceneDesc& scene);

        /// Everything the top-level build needs before a command buffer exists: the instance rows,
        /// the buffer they are written to, the structure itself and the scratch to build it in.
        void prepareTopLevel(const SceneDesc& scene, std::span<const InstanceRecord> records);

        void recordRefit(VkCommandBuffer commands, GpuTimer* timer);
        void recordTopLevel(VkCommandBuffer commands, GpuTimer* timer);

        const Device& mDevice;

        /// Host-written, because a skinned body rewrites its own slice every frame and the build
        /// that reads it runs in the same submit — a host write before a submit needs no barrier.
        ///
        /// **Blocked, so a scene that grows keeps every address it has already handed out.** Nothing
        /// reads these in a shader: a hit gets its vertices back out of the structure through
        /// position fetch, so they are a build input and a write target and nothing else — which is
        /// why there is no table of their addresses beside them.
        BlockedBuffer mPositions{ Shaders::VERTEX_BLOCK, sizeof(osg::Vec3f) };

        BlockedBuffer mIndices{ Shaders::INDEX_BLOCK, sizeof(std::uint32_t) };

        StructureStorage mBottomLevelStorage{ VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            "bottom level structures" };

        /// Where the built micromaps live. Their *inputs* do not: the states and the triangle table
        /// are read once by the build and never again, so they are handed to the batch and freed
        /// with it, the way build scratch is.
        StructureStorage mMicromapStorage{
            VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "opacity micromaps"
        };

        Buffer mTopLevelStorage;

        /// The rows the top level is built from. Rewritten whole every frame, so it is written where
        /// the builder reads it rather than staged into place.
        HostBuffer mInstances;

        std::vector<VkAccelerationStructureKHR> mBottomLevel;

        /// Each mesh's opacity micromap, or nothing where its cutout could not be answered for.
        ///
        /// Indexed by mesh slot beside the structures, and holding `VK_NULL_HANDLE` for every mesh
        /// that is not a cutout — which is nearly all of them.
        std::vector<VkMicromapEXT> mMicromaps;
        std::vector<StructureRoom> mMicromapRooms;
        std::vector<MicromapUsageCounts> mMicromapUsage;

        /// What each mesh's classification came to, kept per mesh rather than summed at build time
        /// so that a mesh released takes its share of the total with it.
        std::vector<MicromapTally> mMicromapTallies;

        /// Where each of those sits in the storage, so a released mesh can give its room back.
        std::vector<StructureRoom> mBottomLevelRooms;

        /// Each of those structures' device address, asked for once when it was made.
        ///
        /// **Not once per instance per frame, which is what this replaced.** A handle lasts from one
        /// `setScene` to the next and its address with it, so a nine-by-nine exterior was making
        /// fifty thousand driver calls a frame to be told the same fifty thousand numbers.
        std::vector<VkDeviceAddress> mBottomLevelAddresses;

        VkAccelerationStructureKHR mTopLevel = VK_NULL_HANDLE;

        /// What each mesh's build asked for, so a rebuild does not have to ask the driver again.
        std::vector<VkDeviceSize> mBuildScratch;

        /// Every mesh slot, for the whole-scene build the constructor does through the same path an
        /// arrival takes. Kept so that path allocates nothing per scene.
        std::vector<Index> mEveryMesh;

        // What one run of `buildMeshes` describes. Members rather than locals because a build info
        // keeps `pGeometries` as a pointer and a range is handed over by address, so both have to
        // outlive the loop that filled them — and because a cell arriving must not allocate five
        // vectors to say so.
        std::vector<VkAccelerationStructureGeometryKHR> mBuildGeometries;

        /// What chains a mesh's micromap onto the geometry it describes. A geometry keeps its
        /// `pNext` as a pointer, so these outlive the loop that filled them for the same reason the
        /// geometries do.
        std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> mBuildMicromapLinks;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mBuilds;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> mBuildRanges;
        std::vector<VkDeviceSize> mBuildSizes;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mLiveBuilds;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> mBuildRangePointers;

        /// Kept across frames rather than made per refit: a device allocation on the frame path is a
        /// stall, and this settles at the high-water mark of whatever the world is showing. It never
        /// shrinks, which is what makes it settle at all.
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

        // Refilled per refit. The build reads `pGeometries` through a pointer, so the geometries are
        // sized before any build info names one.
        std::vector<VkAccelerationStructureGeometryKHR> mRefitGeometries;
        std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> mRefitMicromapLinks;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> mRefitBuilds;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> mRefitRanges;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> mRefitRangePointers;

        // Refilled per placement rather than reallocated: a scene is tens of thousands of these.
        std::vector<VkAccelerationStructureInstanceKHR> mRowScratch;

        // Refilled per build: which material a mesh is worn by, or the sentinel for more than one.
        std::vector<Index> mMaterialOfMesh;

        std::uint32_t mInstanceCount = 0;
        std::uint32_t mCutoutInstanceCount = 0;
        std::uint32_t mMicromappedInstanceCount = 0;

        /// **Two totals, each assigned, because one accumulated.** The bottom levels are made once
        /// and the top level again every frame that moves, so adding both to one figure reported a
        /// scene that grew by its own top level sixty times a second.
        VkDeviceSize mTopLevelBytes = 0;
    };
}
