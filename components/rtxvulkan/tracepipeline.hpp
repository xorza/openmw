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
    /// **The miss and the hit records exist to be named and not to be run.** Stage 1 traces every
    /// ray as an inline query and executes nothing off a hit object, so nothing here past the ray
    /// generation shader is ever invoked — but a hit object records the index of a record, and an
    /// index into a table that has no such record loses the device. `visibility.rmiss` says the rest.
    ///
    /// One record apiece, in the order given: a hit object recording index `i` names entry `i` of
    /// `mHit`, and a missed one names entry `i` of `mMiss`.
    struct TraceShaders
    {
        std::filesystem::path mRaygen;
        std::span<const std::filesystem::path> mMiss;
        std::span<const std::filesystem::path> mHit;
    };

    /// A ray tracing pipeline of one ray generation stage, and the shader binding table a launch
    /// reads it out of.
    ///
    /// **A launch and not a dispatch, for one call.** Nothing here traces a ray through the
    /// pipeline — every ray the trace casts is still an inline query — and there is no recursion.
    /// What the stage buys is `reorderThreadEXT`, which is defined for ray generation and for no
    /// other stage, and `.notes/rtx/ser-plan.md` is the argument.
    ///
    /// `ComputePipeline` is the same object for a dispatch, and the two share `PipelineLayout`.
    class TracePipeline
    {
    public:
        /// Nothing passed outlives the call: every span and path is read into Vulkan's own copies or
        /// into this object's table here.
        ///
        /// @param bindings set zero, which every binding declares the ray generation stage in.
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
