#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "owned.hpp"
#include "pipelinelayout.hpp"

namespace Rtx
{
    class Device;

    /// Which shader stands at each record of a trace's shader binding table.
    ///
    /// One record apiece, in the order given: a hit object naming index `i` runs entry `i` of
    /// `mHit`, and a missed one runs entry `i` of `mMiss`. Which index an instance names is the
    /// shader-table record offset its acceleration structure carries.
    struct TraceShaders
    {
        std::filesystem::path mRaygen;
        std::span<const std::filesystem::path> mMiss;
        std::span<const std::filesystem::path> mHit;

        /// The one any-hit shader every hit group names, or nothing where traversal has no
        /// candidate to ask about.
        ///
        /// **One and not one per group, because the question is the same one.** Whether a candidate
        /// landed on the material or in a hole is a fact about that surface and not about what will
        /// shade it, so a hit group's closest-hit shader is what varies and this is what does not.
        std::filesystem::path mAnyHit;
    };

    /// A ray tracing pipeline and the shader binding table a launch reads it out of.
    ///
    /// **A launch and not a dispatch, for two things.** `reorderThreadEXT` is defined for ray
    /// generation and for no other stage, and a hit object executed there runs a shader picked by
    /// traversal rather than by a branch — so the divergent half of a trace becomes one small
    /// program per kind of hit instead of one kernel holding the union of all of them, each carrying
    /// only its own live state.
    ///
    /// Nothing recurses: the shaders a launch invokes trace again with inline ray queries, which
    /// cost the pipeline's own stack nothing.
    ///
    /// `ComputePipeline` is the same object for a dispatch, and the two share `PipelineLayout`.
    class TracePipeline
    {
    public:
        /// Nothing passed outlives the call: every span and path is read into Vulkan's own copies or
        /// into this object's table here.
        ///
        /// @param bindings set zero, which every binding declares every stage of this pipeline in.
        /// @param laterSets layouts bound after set zero. A pipeline layout has to name every set it
        ///        will ever be handed.
        /// @param shaders the compiled SPIR-V the build wrote, by path.
        /// @param name what a capture calls the pipeline.
        /// @param specialization one word per specialization constant, as `ComputePipeline` takes
        ///        them. Every stage is given the same words.
        TracePipeline(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            std::span<const VkDescriptorSetLayout> laterSets, const TraceShaders& shaders, std::string_view name,
            std::span<const std::uint32_t> specialization = {});

        TracePipeline(const TracePipeline&) = delete;
        TracePipeline& operator=(const TracePipeline&) = delete;

        VkPipeline getHandle() const { return mHandle.get(); }

        /// What descriptors are pushed against.
        VkPipelineLayout getLayout() const { return mLayout.getHandle(); }

        /// Launches `width` by `height` invocations of the ray generation stage.
        void traceRays(VkCommandBuffer commands, std::uint32_t width, std::uint32_t height) const;

    private:
        const Device& mDevice;

        PipelineLayout mLayout;
        Owned<VkPipeline, vkDestroyPipeline> mHandle;

        /// Every group's handle, in video memory the host wrote it straight into.
        Buffer mTable;

        VkStridedDeviceAddressRegionKHR mRaygen{};
        VkStridedDeviceAddressRegionKHR mMiss{};
        VkStridedDeviceAddressRegionKHR mHit{};

        /// Nothing is callable, so this region is empty and the launch is handed it anyway.
        VkStridedDeviceAddressRegionKHR mCallable{};
    };
}
