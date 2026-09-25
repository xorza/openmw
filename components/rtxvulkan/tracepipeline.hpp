#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "pipeline.hpp"

namespace Rtx
{
    class Device;

    /// One closest-hit stage: a module, and the specialization words of its own that follow the
    /// pipeline's in the table `constant_id` indexes — so three stages may be one module under
    /// three settings.
    struct HitShader
    {
        std::filesystem::path mModule;
        std::span<const std::uint32_t> mSpecialization;
    };

    /// Which shader stands at each record of a trace's shader binding table. One miss record
    /// apiece, in the order given: a missed hit object naming index `i` runs entry
    /// `i` of `mMiss`. A closest-hit shader stands behind `mHitRecordsPerShader` records in turn, so
    /// a hit object naming index `i` runs entry `i / mHitRecordsPerShader` of `mHit`. Which index an
    /// instance names is the shader-table record offset its acceleration structure carries, plus
    /// whatever the trace adds.
    struct TraceShaders
    {
        std::filesystem::path mRaygen;
        std::span<const std::filesystem::path> mMiss;
        std::span<const HitShader> mHit;

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

        /// How many bytes of constants the ray generation stage is pushed, at offset zero, or none.
        std::uint32_t mRaygenConstantBytes = 0;
    };

    /// A ray tracing pipeline and the shader binding table a launch reads it out of. A launch and
    /// not a dispatch, because a hit object runs a shader picked by traversal rather than by a
    /// branch, so the divergent half of a trace becomes one small program per kind of hit. Nothing
    /// recurses: the shaders a launch invokes trace again with inline ray queries.
    class TracePipeline : public Pipeline
    {
    public:
        /// Nothing passed outlives the call.
        ///
        /// @param bindings `SET_PASS`, which every binding declares every stage of this pipeline in.
        /// @param shared the shared sets the pipeline reads. A pipeline layout has to name every set it
        ///        will ever be handed.
        /// @param shaders the compiled SPIR-V the build wrote, by path.
        /// @param name what a capture calls the pipeline.
        /// @param specialization one word per specialization constant, as `ComputePipeline` takes
        ///        them. Every stage is given the same words, and a closest-hit stage its own after
        ///        them.
        TracePipeline(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            const SharedSetLayouts& shared, const TraceShaders& shaders, std::string_view name,
            std::span<const std::uint32_t> specialization = {});

        /// Launches `width` by `height` by `depth` invocations of the ray generation stage.
        void traceRays(
            VkCommandBuffer commands, std::uint32_t width, std::uint32_t height, std::uint32_t depth = 1) const;

    private:
        const Device& mDevice;

        /// Every group's handle, in video memory the host wrote it straight into.
        Buffer mTable;

        VkStridedDeviceAddressRegionKHR mRaygen{};
        VkStridedDeviceAddressRegionKHR mMiss{};
        VkStridedDeviceAddressRegionKHR mHit{};

        /// Nothing is callable, so this region is empty and the launch is handed it anyway.
        VkStridedDeviceAddressRegionKHR mCallable{};
    };
}
