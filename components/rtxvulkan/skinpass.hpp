#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include <vulkan/vulkan_core.h>

#include <components/rtx/slots.hpp>

#include "computepipeline.hpp"
#include "skintables.hpp"
#include "slottable.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;
    class SceneDesc;

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

        /// Records `slot`'s dispatches into `commands`: every mesh `poses` owes, its rows or
        /// weights written into `tables`' copy first, and one barrier after them for the build and
        /// the trace. True where anything was recorded. `poses` is indexed by
        /// `MeshRange::mBindOffset` and `normals` by the scene's own vertex offset, because a hit
        /// reads a normal and never a position. The write-after-read against the copy's previous
        /// reader is the fence the caller waited.
        bool record(VkCommandBuffer commands, const SceneDesc& scene, FrameSlot slot, SkinTables& tables,
            SlotBlocks& poses, SlotBlocks& normals, GpuTimer* timer) const;

        /// The same for the deforming meshes among `arrived`, and those alone, into `slot`'s copy,
        /// for the build over them: nothing is owed or paid, and the rows go into runs the meshes
        /// were only just given. An arrival does not wait the frames in flight out, so it may not
        /// touch a row a placement in flight reads — which posing every mesh the copy owed did.
        /// Untimed: the frame's report carries one `skin` zone and it is the placement's.
        bool recordArrived(VkCommandBuffer commands, const SceneDesc& scene, FrameSlot slot,
            std::span<const Index> arrived, SkinTables& tables, SlotBlocks& poses, SlotBlocks& normals) const;

    private:
        /// One mesh's dispatch, binding whichever of the two pipelines it needs where `bound` is
        /// not already it. The mesh deforms and has vertices, which the caller asked first.
        void pose(VkCommandBuffer commands, const SceneDesc& scene, FrameSlot slot, Index mesh, SkinTables& tables,
            SkinTables::Rows rows, BlockedBuffer& into, BlockedBuffer& normalsInto,
            const ComputePipeline*& bound) const;

        ComputePipeline mSkin;
        ComputePipeline mMorph;
    };
}
