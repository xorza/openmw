#pragma once

#include <filesystem>
#include <span>

#include <vulkan/vulkan_core.h>

#include <components/rtx/runs.hpp>

#include "blockedbuffer.hpp"
#include "computepipeline.hpp"
#include "frameslots.hpp"
#include "skintables.hpp"
#include "slottable.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;
    class SceneDesc;

    /// What one scene's posing is done into. A record and not an argument list, because `mPoses`
    /// and `mNormals` are the same type and the two tables they name are not interchangeable.
    struct Skinning
    {
        const SceneDesc& mScene;

        /// Which copy of the tables is written.
        FrameSlot mSlot;

        /// What this scene's bodies are posed from.
        SkinTables& mTables;

        /// Where the posed vertices go, indexed by `MeshRange::mBindOffset`, and where the posed
        /// normals go, indexed by the scene's own vertex offset — because a hit reads a normal
        /// and never a position.
        SlotBlocks& mPoses;
        SlotBlocks& mNormals;

        /// Null where the run is not being timed, and never read by `recordArrived`.
        GpuTimer* mTimer = nullptr;
    };

    /// Poses every deforming mesh a slot's copies owe, on the device, ahead of the refit over
    /// them: per bone on the host and per vertex on the device, where a body skinned on the
    /// processor was compared, copied, bounded and written across the bus twice every frame.
    /// `SlotBlocks` says which runs each copy owes — this frame's movers and the ones the frame
    /// before last missed — and one dispatch writes both tables. Shared by every scene; what
    /// differs per scene is `SkinTables`.
    class SkinPass
    {
    public:
        SkinPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Records `what.mSlot`'s dispatches into `commands`: every mesh `what.mPoses` owes, its
        /// rows or weights written into the tables' copy first, and one barrier after them for
        /// the build and the trace. True where anything was recorded. The write-after-read
        /// against the copy's previous reader is the fence the caller waited.
        bool record(VkCommandBuffer commands, const Skinning& what) const;

        /// The same for the deforming meshes among `arrived`, and those alone, into the copy, for
        /// the build over them: nothing is owed or paid, and the rows are the ones
        /// `SkinTables::extend` staged there. An arrival does not wait the frames in flight out,
        /// so it may not write a row a placement in flight reads — which posing every mesh the
        /// copy owed did. `what.mTimer` is not read: the frame's report carries one `skin` zone
        /// and it is the placement's.
        bool recordArrived(VkCommandBuffer commands, const Skinning& what, std::span<const Index> arrived) const;

    private:
        /// Whether a dispatch writes the mesh's pose into the copy first, which a placement does,
        /// or reads the pose `SkinTables::extend` staged there for an arrival.
        enum class Rows
        {
            Written,
            Staged,
        };

        /// One mesh's dispatch, binding whichever of the two pipelines it needs where `bound` is
        /// not already it. The mesh deforms and has vertices, which the caller asked first.
        /// `into` and `normalsInto` are `what`'s two tables at its own slot, taken once by the
        /// caller rather than per mesh.
        void pose(VkCommandBuffer commands, const Skinning& what, Index mesh, Rows rows, BlockedBuffer& into,
            BlockedBuffer& normalsInto, const ComputePipeline*& bound) const;

        ComputePipeline mSkin;
        ComputePipeline mMorph;
    };
}
