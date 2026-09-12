#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vulkan/vulkan_core.h>

#include "computepipeline.hpp"

namespace Rtx
{
    /// How many workgroups of `workgroup` lanes cover `extent` of them.
    ///
    /// **One statement for every pass, because a dispatch that fell a group short would leave a
    /// stripe of the frame untouched** — and a rounding written once is one a test can hold.
    constexpr std::uint32_t groupsFor(std::uint32_t extent, std::uint32_t workgroup)
    {
        return (extent + workgroup - 1) / workgroup;
    }

    /// One binding of set zero, visible to the compute stage.
    constexpr VkDescriptorSetLayoutBinding computeBinding(std::uint32_t slot, VkDescriptorType type)
    {
        return VkDescriptorSetLayoutBinding{ slot, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    }

    /// `Count` bindings of one type, numbered from zero. A pass that mixes descriptor types builds
    /// its own list out of `computeBinding`.
    template <std::size_t Count>
    constexpr std::array<VkDescriptorSetLayoutBinding, Count> computeBindings(VkDescriptorType type)
    {
        std::array<VkDescriptorSetLayoutBinding, Count> bindings{};
        for (std::uint32_t slot = 0; slot < Count; ++slot)
            bindings[slot] = computeBinding(slot, type);

        return bindings;
    }

    /// One descriptor write. **`info` is read when the write is submitted and not here**, so it has
    /// to outlive the array this goes into — which is what keeps every caller's infos a local.
    constexpr VkWriteDescriptorSet imageWrite(std::uint32_t binding, const VkDescriptorImageInfo& info,
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
    {
        return VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = &info,
        };
    }

    constexpr VkWriteDescriptorSet bufferWrite(std::uint32_t binding, const VkDescriptorBufferInfo& info,
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
    {
        return VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pBufferInfo = &info,
        };
    }

    /// A write per image, binding `i` from image `i`, each taking the type its own binding was
    /// declared with, so a binding that changes kind cannot be a silent mismatch. `images` has to
    /// outlive the writes, as `imageWrite` says.
    template <std::size_t Count>
    std::array<VkWriteDescriptorSet, Count> imageWrites(const std::array<VkDescriptorImageInfo, Count>& images,
        const std::array<VkDescriptorSetLayoutBinding, Count>& bindings)
    {
        std::array<VkWriteDescriptorSet, Count> writes{};
        for (std::uint32_t at = 0; at < Count; ++at)
            writes[at] = imageWrite(at, images[at], bindings[at].descriptorType);

        return writes;
    }

    template <std::size_t Count>
    std::array<VkWriteDescriptorSet, Count> storageImageWrites(const std::array<VkDescriptorImageInfo, Count>& images)
    {
        std::array<VkWriteDescriptorSet, Count> writes{};
        for (std::uint32_t at = 0; at < Count; ++at)
            writes[at] = imageWrite(at, images[at]);

        return writes;
    }

    /// Orders one dispatch's writes against what reads or writes them next.
    inline void handOver(VkCommandBuffer commands, VkPipelineStageFlags2 from, VkAccessFlags2 wrote,
        VkPipelineStageFlags2 to, VkAccessFlags2 reads)
    {
        const VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = from,
            .srcAccessMask = wrote,
            .dstStageMask = to,
            .dstAccessMask = reads,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    /// Binds, pushes and launches: the four calls every compute pass in this backend ends with.
    ///
    /// @param constants the whole push range, at offset zero. Taken by reference and copied by
    ///        Vulkan before this returns.
    template <class Constants>
    void dispatch(VkCommandBuffer commands, const ComputePipeline& pipeline,
        std::span<const VkWriteDescriptorSet> writes, const Constants& constants, std::uint32_t groupsX,
        std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1)
    {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getHandle());
        vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0,
            static_cast<std::uint32_t>(writes.size()), writes.data());
        vkCmdPushConstants(
            commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commands, groupsX, groupsY, groupsZ);
    }
}
