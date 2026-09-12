#include "exposurepass.hpp"

#include <array>
#include <cstdint>

#include "dispatch.hpp"
#include "image.hpp"

namespace Rtx
{
    namespace
    {
        /// The frame in, the histogram out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sHistogramBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        };

        /// The histogram in, the one float out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sReduceBindings
            = computeBindings<2>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    }

    ExposurePass::ExposurePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mHistogramPipeline(device, sHistogramBindings, sizeof(Shaders::HistogramConstants), {},
            shaderDirectory / "histogram.comp.spv", "histogram")
        , mReducePipeline(device, sReduceBindings, sizeof(Shaders::ExposureConstants), {},
              shaderDirectory / "exposure.comp.spv", "exposure")
        , mHistogram(Buffer::deviceLocal(device, Shaders::EXPOSURE_BINS * sizeof(std::uint32_t),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        , mExposure(Buffer::deviceLocal(
              device, sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        , mPicture(Buffer::hostWritten(device, sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
    {
        mPicture.writable<float>(0, 1).front() = 1.0f;
    }

    void ExposurePass::beforeWrite(VkCommandBuffer commands) const
    {
        // Against the previous frame and not this one: two frames in flight share one set of these
        // buffers, so the measurement about to overwrite them may start while the curve reading
        // them is still running. An execution dependency is all a write-after-read needs.
        const std::array<VkBufferMemoryBarrier2, 2> barriers{
            VkBufferMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = mHistogram.getHandle(),
                .size = VK_WHOLE_SIZE,
            },
            VkBufferMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask
                = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                // Read as well as written, because the reduction now moves the previous frame's
                // exposure toward this frame's measurement: the write before it has to be visible
                // and not merely ordered.
                .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
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

    void ExposurePass::record(
        VkCommandBuffer commands, const Image& frame, float elapsedSeconds, bool reset, float bias) const
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

        const std::array<VkWriteDescriptorSet, 2> binning{ imageWrite(0, source), bufferWrite(1, histogram) };

        const Shaders::HistogramConstants extent{
            .mWidth = frame.getWidth(),
            .mHeight = frame.getHeight(),
        };

        dispatch(commands, mHistogramPipeline, binning, extent, groupsFor(extent.mWidth, Shaders::HISTOGRAM_WORKGROUP),
            groupsFor(extent.mHeight, Shaders::HISTOGRAM_WORKGROUP));

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
            .mElapsed = elapsedSeconds,
            .mReset = reset ? 1u : 0u,
            .mBias = bias,
        };

        // One group, because the reduction is over the bins and the bins are one workgroup's worth.
        dispatch(commands, mReducePipeline, reducing, counted, 1);

        handOver(commands);
    }
}
