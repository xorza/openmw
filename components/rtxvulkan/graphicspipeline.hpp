#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

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

        /// The format of the one colour attachment.
        ///
        /// **Dynamic rendering, so there is no render pass and no framebuffer.** The pipeline is
        /// told what it will be drawing into and the recording says which image that is — which is
        /// the difference between an object per target size and one for the run.
        VkFormat mColourFormat = VK_FORMAT_UNDEFINED;

        Blend mBlend = Blend::None;

        std::filesystem::path mVertexModule;
        std::filesystem::path mFragmentModule;

        /// What a capture calls the pipeline.
        std::string_view mName;
    };

    /// A graphics pipeline, the descriptor set layout it is addressed through, and the pipeline
    /// layout that ties the two together.
    ///
    /// **The three are one object because they fail as one**, for the reason `ComputePipeline`
    /// gives: a constructor that throws gets no destructor, so whatever the earlier handles took has
    /// to be given back before the failure leaves. As a member, that unwind is written once here
    /// rather than in every pass.
    ///
    /// **The first thing in this backend that is not compute.** Everything that makes the picture is
    /// dispatched — the trace, the denoiser, exposure, the curve, the composite. This exists because
    /// a GUI is triangles over a finished frame and there is nothing to be gained by tracing a font
    /// atlas.
    class GraphicsPipeline
    {
    public:
        GraphicsPipeline(const Device& device, const GraphicsPipelineOptions& options);
        ~GraphicsPipeline();

        GraphicsPipeline(const GraphicsPipeline&) = delete;
        GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

        VkPipeline getHandle() const { return mHandle; }

        /// What descriptors are pushed against and push constants are written through.
        VkPipelineLayout getLayout() const { return mLayout; }

    private:
        void destroy();

        const Device& mDevice;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mHandle = VK_NULL_HANDLE;
    };
}
