#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "handles.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// How what a pipeline draws reaches what is already in the attachment.
    enum class Blend
    {
        /// Straight through: what is written replaces what is there.
        None,

        /// Source alpha over what is there, and the source's own alpha accumulated the way a
        /// premultiplied composite wants it.
        Over,

        /// Added to what is there, scaled by its own alpha.
        Additive,
    };

    /// What a raster pipeline is made of that a compute one has no equivalent for.
    ///
    /// **No span outlives the call.** Every one is read into Vulkan's own copies inside the
    /// constructor, which is what lets a caller pass the address of one of its own locals.
    struct GraphicsPipelineOptions
    {
        /// Set zero, which is always a push descriptor set: nothing in this renderer wants a
        /// descriptor pool on the frame path.
        std::span<const VkDescriptorSetLayoutBinding> mBindings;

        /// The whole range, at offset zero, visible to both stages. Zero where there is none.
        std::uint32_t mPushConstantBytes = 0;

        std::span<const VkVertexInputBindingDescription> mVertexBindings;
        std::span<const VkVertexInputAttributeDescription> mVertexAttributes;

        /// The format of the one colour attachment. Dynamic rendering, so there is no render pass
        /// and no framebuffer, and one object serves every target size.
        VkFormat mColourFormat = VK_FORMAT_UNDEFINED;

        Blend mBlend = Blend::None;

        std::filesystem::path mVertexModule;
        std::filesystem::path mFragmentModule;

        /// What a capture calls the pipeline.
        std::string_view mName;
    };

    /// A graphics pipeline and the layout it is addressed through, one object for the reason
    /// `ComputePipeline` gives. The one thing in this backend that is not compute, because there
    /// is nothing to be gained by tracing a font atlas.
    class GraphicsPipeline
    {
    public:
        GraphicsPipeline(const Device& device, const GraphicsPipelineOptions& options);

        VkPipeline getHandle() const { return mHandle.get(); }

        /// What descriptors are pushed against and push constants are written through.
        VkPipelineLayout getLayout() const { return mLayout.getHandle(); }

    private:
        PipelineLayout mLayout;
        Owned<VkPipeline, vkDestroyPipeline> mHandle;
    };
}
