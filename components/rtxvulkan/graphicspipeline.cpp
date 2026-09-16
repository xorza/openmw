#include "graphicspipeline.hpp"

#include <array>
#include <cassert>
#include <cstdint>

#include "device.hpp"
#include "handles.hpp"
#include "image.hpp"
#include "result.hpp"

namespace Rtx
{
    GraphicsPipeline::GraphicsPipeline(const Device& device, const GraphicsPipelineOptions& options)
        : Pipeline(PipelineLayout(device, options.mBindings,
                       VkPushConstantRange{ .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           .size = options.mPushConstantBytes },
                       {}),
            VK_PIPELINE_BIND_POINT_GRAPHICS)
    {
        const ShaderModule vertex = loadShaderModule(device, options.mVertexModule);
        const ShaderModule fragment = loadShaderModule(device, options.mFragmentModule);

        const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertex.get(),
                .pName = "main",
            },
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragment.get(),
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
            .topology = options.mTopology,
        };

        // Both dynamic: the target is resized more often than the pipeline is worth rebuilding.
        const VkPipelineViewportStateCreateInfo viewport{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };

        // No culling. What is drawn here is two-dimensional or a debug mesh, and its winding says
        // nothing; a flipped viewport would otherwise reverse the face of every triangle at once.
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
            .colorWriteMask
            = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
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
            .layout = mLayout.getHandle(),
        };
        checkVk(vkCreateGraphicsPipelines(device.getHandle(), device.getPipelineCache(), 1, &pipeline, nullptr,
                    mHandle.put(device.getHandle())),
            "vkCreateGraphicsPipelines");

        device.setName(mHandle.get(), options.mName);
        device.reportPipeline(mHandle.get(), options.mName);
    }

    void beginDrawingOver(const VkCommandBuffer commands, const Image& target, const ClipUp up)
    {
        assert((target.getUsage() & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);

        const VkRenderingAttachmentInfo colour{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = target.getView(),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        const VkRenderingInfo rendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = { { 0, 0 }, { target.getWidth(), target.getHeight() } },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colour,
        };

        vkCmdBeginRendering(commands, &rendering);

        // Upside down where the pass writes OpenGL's clip space: flipping the viewport rather than
        // the vertices leaves its vertex shader a pass-through and costs nothing at all.
        const auto height = static_cast<float>(target.getHeight());
        const VkViewport viewport{
            .x = 0.0f,
            .y = up == ClipUp::Up ? height : 0.0f,
            .width = static_cast<float>(target.getWidth()),
            .height = up == ClipUp::Up ? -height : height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        const VkRect2D scissor{ { 0, 0 }, { target.getWidth(), target.getHeight() } };

        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
    }
}
