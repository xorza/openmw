#pragma once

#include <cstdint>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include "computepipeline.hpp"
#include "slottable.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;
    class SceneDesc;
    class SkinTables;

    /// Poses every deforming mesh a slot's copies owe, on the device, ahead of the refit over them.
    ///
    /// **What the host used to do per vertex, done per bone.** A skinned body was skinned on the
    /// processor, compared vertex by vertex against the pose before, copied, walked for its bounds
    /// and written across the bus twice — positions for the refit and normals for the hit — every
    /// frame. Now the host writes a few dozen rows per body and one dispatch per body computes the
    /// vertices into the slot's copy of the positions and the normals, where the refit and the hit
    /// already read them.
    ///
    /// **The positions' own account is what drives it.** `SlotBlocks` says which runs each copy
    /// owes; a copy owes a mesh whose pose changed since that copy was last written, which is
    /// exactly the set of dispatches it needs — this frame's movers and the ones the frame before
    /// last missed. One dispatch writes both tables, so the normals keep no account of their own.
    ///
    /// **Shared by every scene**, like the trace: two pipelines, and nothing about them depends on
    /// which scene they pose. What differs per scene is `SkinTables`.
    class SkinPass
    {
    public:
        SkinPass(const Device& device, const std::filesystem::path& shaderDirectory);

        SkinPass(const SkinPass&) = delete;
        SkinPass& operator=(const SkinPass&) = delete;

        /// Records `slot`'s dispatches into `commands`: every mesh `positions` owes, its rows or
        /// weights written into `tables`' copy first, and one barrier after them for the build and
        /// the trace. True where anything was recorded.
        ///
        /// **Into `slot`'s copy, which the caller has made sure no frame is still reading**, and
        /// before the refit that reads what this wrote. The write-after-read against the copy's
        /// previous reader is the fence the caller waited; the read-after-write into the refit and
        /// the trace is the barrier here.
        bool record(VkCommandBuffer commands, const SceneDesc& scene, std::uint32_t slot, SkinTables& tables,
            SlotBlocks& positions, SlotBlocks& normals, GpuTimer* timer) const;

    private:
        ComputePipeline mSkin;
        ComputePipeline mMorph;
    };
}
