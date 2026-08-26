#include "exposurepass.hpp"

#include <array>

#include "image.hpp"

namespace Rtx
{
    namespace
    {
        /// The frame in, the histogram out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sHistogramBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };

        /// The histogram in, the one float out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sReduceBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };

        std::uint32_t groupsFor(std::uint32_t extent)
        {
            return (extent + Shaders::HISTOGRAM_WORKGROUP - 1) / Shaders::HISTOGRAM_WORKGROUP;
        }

        VkWriteDescriptorSet bufferWrite(std::uint32_t binding, const VkDescriptorBufferInfo& info)
        {
            return VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = binding,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &info,
            };
        }
    }

    ExposurePass::ExposurePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mHistogramPipeline(device, sHistogramBindings, sizeof(Shaders::HistogramConstants), {},
            shaderDirectory / "histogram.comp.spv", "histogram")
        , mReducePipeline(device, sReduceBindings, sizeof(Shaders::ExposureConstants), {},
              shaderDirectory / "exposure.comp.spv", "exposure")
        , mHistogram(device, Shaders::EXPOSURE_BINS * sizeof(std::uint32_t),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , mExposure(device, sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    {
    }

    void ExposurePass::beforeWrite(VkCommandBuffer commands) const
    {
        // **Against the previous frame and not this one.** A window keeps two frames in flight and
        // there is one set of these buffers, so the measurement about to overwrite them may start
        // while the curve reading them for the frame before is still running. A barrier orders
        // against everything already submitted to the queue, which is the whole of what a
        // write-after-read needs; nothing has to be made visible.
        const std::array<VkBufferMemoryBarrier2, 2> barriers{
            VkBufferMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = mHistogram.getHandle(),
                .size = VK_WHOLE_SIZE,
            },
            VkBufferMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = mExposure.getHandle(),
                .size = VK_WHOLE_SIZE,
            },
        };

        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()),
            .pBufferMemoryBarriers = barriers.data(),
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    void ExposurePass::handOver(VkCommandBuffer commands) const
    {
        const VkBufferMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mExposure.getHandle(),
            .size = VK_WHOLE_SIZE,
        };

        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    void ExposurePass::recordFixed(VkCommandBuffer commands, float value) const
    {
        beforeWrite(commands);

        // Four bytes, so this is an inline write into the command buffer rather than a staging copy.
        vkCmdUpdateBuffer(commands, mExposure.getHandle(), 0, sizeof(value), &value);
        handOver(commands);
    }

    void ExposurePass::record(VkCommandBuffer commands, const Image& frame) const
    {
        beforeWrite(commands);

        // Cleared here and not in a shader: the workgroups accumulate into it, so one of them
        // zeroing it would race with the rest.
        vkCmdFillBuffer(commands, mHistogram.getHandle(), 0, VK_WHOLE_SIZE, 0);

        const VkBufferMemoryBarrier2 cleared{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mHistogram.getHandle(),
            .size = VK_WHOLE_SIZE,
        };

        VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &cleared,
        };
        vkCmdPipelineBarrier2(commands, &dependency);

        const VkDescriptorImageInfo source{ VK_NULL_HANDLE, frame.getView(), VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorBufferInfo histogram{ mHistogram.getHandle(), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo exposure{ mExposure.getHandle(), 0, VK_WHOLE_SIZE };

        const std::array<VkWriteDescriptorSet, 2> binning{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &source,
            },
            bufferWrite(1, histogram),
        };

        const Shaders::HistogramConstants extent{
            .mWidth = frame.getWidth(),
            .mHeight = frame.getHeight(),
        };

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mHistogramPipeline.getHandle());
        vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mHistogramPipeline.getLayout(), 0,
            static_cast<std::uint32_t>(binning.size()), binning.data());
        vkCmdPushConstants(
            commands, mHistogramPipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(extent), &extent);
        vkCmdDispatch(commands, groupsFor(extent.mWidth), groupsFor(extent.mHeight), 1);

        // The reduction has to see every pixel's contribution before it divides by the total, which
        // is what this dispatch boundary is for.
        const VkBufferMemoryBarrier2 binned{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mHistogram.getHandle(),
            .size = VK_WHOLE_SIZE,
        };
        dependency.pBufferMemoryBarriers = &binned;
        vkCmdPipelineBarrier2(commands, &dependency);

        const std::array<VkWriteDescriptorSet, 2> reducing{ bufferWrite(0, histogram), bufferWrite(1, exposure) };

        const Shaders::ExposureConstants counted{
            .mPixels = frame.getWidth() * frame.getHeight(),
        };

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mReducePipeline.getHandle());
        vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mReducePipeline.getLayout(), 0,
            static_cast<std::uint32_t>(reducing.size()), reducing.data());
        vkCmdPushConstants(
            commands, mReducePipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(counted), &counted);
        vkCmdDispatch(commands, 1, 1, 1);

        handOver(commands);
    }
}
