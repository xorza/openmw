#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "handles.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// Which shader stands at each record of a trace's shader binding table.
    ///
    /// One miss record apiece, in the order given: a missed hit object naming index `i` runs entry
    /// `i` of `mMiss`. A closest-hit shader stands behind `mHitRecordsPerShader` records in turn, so
    /// a hit object naming index `i` runs entry `i / mHitRecordsPerShader` of `mHit`. Which index an
    /// instance names is the shader-table record offset its acceleration structure carries, plus
    /// whatever the trace adds.
    struct TraceShaders
    {
        std::filesystem::path mRaygen;
        std::span<const std::filesystem::path> mMiss;
        std::span<const std::filesystem::path> mHit;

        /// How many records each closest-hit shader stands behind.
        std::uint32_t mHitRecordsPerShader = 1;

        /// What each hit record carries after its handle, one block per record in record order,
        /// every block the same size — or nothing. In the record and not in the payload, because a
        /// record is read by the shader the hit object names, whatever sorted the threads between.
        std::span<const std::byte> mHitRecordData;

        /// The one any-hit shader every hit group names, or nothing where traversal has no
        /// candidate to ask about. One and not one per group, because whether a candidate landed in
        /// a hole is a fact about the surface and not about what will shade it.
        std::filesystem::path mAnyHit;
    };

    /// A ray tracing pipeline and the shader binding table a launch reads it out of. A launch and
    /// not a dispatch, because a hit object runs a shader picked by traversal rather than by a
    /// branch, so the divergent half of a trace becomes one small program per kind of hit. Nothing
    /// recurses: the shaders a launch invokes trace again with inline ray queries.
    class TracePipeline
    {
    public:
        /// Nothing passed outlives the call.
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
