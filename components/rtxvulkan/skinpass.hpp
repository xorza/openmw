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

    private:
        ComputePipeline mSkin;
        ComputePipeline mMorph;
    };
}
