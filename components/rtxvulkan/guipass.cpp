#include "guipass.hpp"

#include <array>
#include <cassert>
#include <cstddef>

#include <components/rtx/renderer.hpp>

#include "device.hpp"
#include "image.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// One texture, pushed per batch. Nothing else: a GUI vertex carries its own colour and
        /// its own position, and there is no transform to hand down.
        constexpr std::array<VkDescriptorSetLayoutBinding, 1> sBindings{
            VkDescriptorSetLayoutBinding{
                0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        };

        constexpr std::array<VkVertexInputBindingDescription, 1> sVertexBindings{
            VkVertexInputBindingDescription{ 0, sizeof(GuiVertex), VK_VERTEX_INPUT_RATE_VERTEX },
        };

        /// **The colour is four bytes read as a normalised vector by the hardware**, which is what
        /// makes MyGUI's own packing free to consume: `ColourABGR` puts red in the low byte, which
        /// is what `R8G8B8A8_UNORM` reads first.
        constexpr std::array<VkVertexInputAttributeDescription, 3> sVertexAttributes{
            VkVertexInputAttributeDescription{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GuiVertex, mX) },
            VkVertexInputAttributeDescription{ 1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GuiVertex, mColour) },
            VkVertexInputAttributeDescription{ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GuiVertex, mU) },
        };

        GraphicsPipelineOptions describePipeline(
            const std::filesystem::path& shaderDirectory, VkFormat targetFormat, Blend blend)
        {
            GraphicsPipelineOptions options;
            options.mBindings = sBindings;
            options.mVertexBindings = sVertexBindings;
            options.mVertexAttributes = sVertexAttributes;
            options.mColourFormat = targetFormat;
            options.mBlend = blend;
            options.mVertexModule = shaderDirectory / "gui.vert.spv";
            options.mFragmentModule = shaderDirectory / "gui.frag.spv";
            options.mName = blend == Blend::Additive ? "gui additive" : "gui";
            return options;
        }
    }

    GuiPass::GuiPass(const Device& device, const std::filesystem::path& shaderDirectory, VkFormat targetFormat)
        : mDevice(device)
        , mOver(device, describePipeline(shaderDirectory, targetFormat, Blend::Over))
        , mAdditive(device, describePipeline(shaderDirectory, targetFormat, Blend::Additive))
    {
        // After the pipelines, not before: a member that throws while being constructed leaves the
        // ones already built to their own destructors, and a handle made in this body would have
        // none. Nothing after this can throw.
        const VkSamplerCreateInfo sampler{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            // Clamped, because a widget's atlas entry runs to the edge of what it was given and
            // wrapping would fetch the glyph next to it.
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = VK_LOD_CLAMP_NONE,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        };
        checkVk(vkCreateSampler(mDevice.getHandle(), &sampler, nullptr, &mSampler), "vkCreateSampler");
    }

    GuiPass::~GuiPass()
    {
        if (mSampler != VK_NULL_HANDLE)
            vkDestroySampler(mDevice.getHandle(), mSampler, nullptr);
    }

    void GuiPass::record(
        VkCommandBuffer commands, const Image& target, VkBuffer vertices, std::span<const GuiDraw> draws) const
    {
        assert((target.getUsage() & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);

        if (draws.empty())
            return;

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

        // **Upside down on purpose.** MyGUI computes its vertices for a clip space with +Y up,
        // which is OpenGL's; Vulkan's points the other way. Flipping the viewport rather than the
        // vertices leaves the vertex shader a pass-through and costs nothing at all.
        const VkViewport viewport{
            .x = 0.0f,
            .y = static_cast<float>(target.getHeight()),
            .width = static_cast<float>(target.getWidth()),
            .height = -static_cast<float>(target.getHeight()),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        const VkRect2D scissor{ { 0, 0 }, { target.getWidth(), target.getHeight() } };

        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commands, 0, 1, &vertices, &offset);

        const GraphicsPipeline* bound = nullptr;

        for (const GuiDraw& draw : draws)
        {
            const GraphicsPipeline& pipeline = draw.mBlend == Blend::Additive ? mAdditive : mOver;
            if (&pipeline != bound)
            {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.getHandle());
                bound = &pipeline;
            }

            const VkDescriptorImageInfo texture{ mSampler, draw.mTexture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &texture,
            };

            // Against the layout of the pipeline that is bound: the two are identical, but a push
            // is only defined against the one in force.
            vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.getLayout(), 0, 1, &write);
            vkCmdDraw(commands, draw.mVertexCount, 1, draw.mFirstVertex, 0);
        }

        vkCmdEndRendering(commands);
    }
}
