#include "graphicspipeline.hpp"

#include <array>
#include <vector>

#include "device.hpp"
#include "result.hpp"
#include "shadermodule.hpp"

namespace Rtx
{
    GraphicsPipeline::GraphicsPipeline(const Device& device, const GraphicsPipelineOptions& options)
        : mDevice(device)
    {
        try
        {
            const VkDescriptorSetLayoutCreateInfo layout{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                .bindingCount = static_cast<std::uint32_t>(options.mBindings.size()),
                .pBindings = options.mBindings.data(),
            };
            checkVk(vkCreateDescriptorSetLayout(mDevice.getHandle(), &layout, nullptr, &mSetLayout),
                "vkCreateDescriptorSetLayout");

            const VkPushConstantRange range{
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                .size = options.mPushConstantBytes,
            };
            const VkPipelineLayoutCreateInfo pipelineLayout{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &mSetLayout,
                // A range of zero bytes is not a legal one to ask for, so a pipeline with no push
                // constants declares no range at all rather than an empty one.
                .pushConstantRangeCount = options.mPushConstantBytes > 0 ? 1u : 0u,
                .pPushConstantRanges = &range,
            };
            checkVk(vkCreatePipelineLayout(mDevice.getHandle(), &pipelineLayout, nullptr, &mLayout),
                "vkCreatePipelineLayout");

            const ShaderModule vertex(mDevice, options.mVertexModule);
            const ShaderModule fragment(mDevice, options.mFragmentModule);

            const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = vertex.getHandle(),
                    .pName = "main",
                },
                VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = fragment.getHandle(),
                    .pName = "main",
                },
            };

            const VkPipelineVertexInputStateCreateInfo vertexInput{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                .vertexBindingDescriptionCount = static_cast<std::uint32_t>(options.mVertexBindings.size()),
                .pVertexBindingDescriptions = options.mVertexBindings.data(),
                .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(options.mVertexAttributes.size()),
                .pVertexAttributeDescriptions = options.mVertexAttributes.data(),
            };

            const VkPipelineInputAssemblyStateCreateInfo assembly{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            };

            // Both dynamic: the target is resized more often than the pipeline is worth rebuilding.
            const VkPipelineViewportStateCreateInfo viewport{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                .viewportCount = 1,
                .scissorCount = 1,
            };

            // **No culling.** What is drawn here is two-dimensional and its winding says nothing;
            // a flipped viewport would otherwise reverse the face of every triangle at once.
            const VkPipelineRasterizationStateCreateInfo raster{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .polygonMode = VK_POLYGON_MODE_FILL,
                .cullMode = VK_CULL_MODE_NONE,
                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                .lineWidth = 1.0f,
            };

            const VkPipelineMultisampleStateCreateInfo multisample{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            };

            // Additive keeps what is under it whole and adds to it; `Over` takes that much of it
            // away first. The two differ in this one factor and nothing else.
            const VkBlendFactor destination
                = options.mBlend == Blend::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

            const VkPipelineColorBlendAttachmentState attachment{
                .blendEnable = options.mBlend != Blend::None ? VK_TRUE : VK_FALSE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                .dstColorBlendFactor = destination,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = destination,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
                    | VK_COLOR_COMPONENT_A_BIT,
            };
            const VkPipelineColorBlendStateCreateInfo blend{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments = &attachment,
            };

            constexpr std::array<VkDynamicState, 2> dynamicStates{
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR,
            };
            const VkPipelineDynamicStateCreateInfo dynamic{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
                .pDynamicStates = dynamicStates.data(),
            };

            const VkPipelineRenderingCreateInfo rendering{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .colorAttachmentCount = 1,
                .pColorAttachmentFormats = &options.mColourFormat,
            };

            const VkGraphicsPipelineCreateInfo pipeline{
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .pNext = &rendering,
                // Asked for at creation because it cannot be asked for afterwards, and paid at
                // creation rather than at draw. `Device::reportPipeline` says what it buys.
                .flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR,
                .stageCount = static_cast<std::uint32_t>(stages.size()),
                .pStages = stages.data(),
                .pVertexInputState = &vertexInput,
                .pInputAssemblyState = &assembly,
                .pViewportState = &viewport,
                .pRasterizationState = &raster,
                .pMultisampleState = &multisample,
                .pColorBlendState = &blend,
                .pDynamicState = &dynamic,
                .layout = mLayout,
            };
            checkVk(vkCreateGraphicsPipelines(
                        mDevice.getHandle(), mDevice.getPipelineCache(), 1, &pipeline, nullptr, &mHandle),
                "vkCreateGraphicsPipelines");

            mDevice.setName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(mHandle), options.mName);
            mDevice.reportPipeline(mHandle, options.mName);
        }
        catch (...)
        {
            destroy();
            throw;
        }
    }

    GraphicsPipeline::~GraphicsPipeline()
    {
        destroy();
    }

    void GraphicsPipeline::destroy()
    {
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice.getHandle(), mHandle, nullptr);
        if (mLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(mDevice.getHandle(), mLayout, nullptr);
        if (mSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(mDevice.getHandle(), mSetLayout, nullptr);
    }
}
