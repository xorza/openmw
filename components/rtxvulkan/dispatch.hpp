#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vulkan/vulkan_core.h>

#include "computepipeline.hpp"
#include "pipeline.hpp"

namespace Rtx
{
    /// How many workgroups of `workgroup` lanes cover `extent` of them. One statement for every
    /// pass, because a dispatch that fell a group short would leave a stripe of the frame
    /// untouched — and a rounding written once is one a test can hold.
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

    /// The writes one push or one set update is made of, and the infos they point at, in one
    /// object: a write names its info by address, so the two live together and neither moves —
    /// which is why this is neither copied nor moved, and why it is a local of the pass that
    /// fills it. Appended in binding order, which a debug build asserts, so a binding a shader
    /// grew and a pass did not is a count the pass can check, and two writes the pass swapped are a
    /// failure and not two descriptors of one type landing on each other's slot.
    ///
    /// @tparam Bindings how many writes there are room for.
    /// @tparam Images how many image infos, where a binding is an array of them.
    template <std::size_t Bindings, std::size_t Images = Bindings>
    class DescriptorWrites
    {
    public:
        /// For a push: no set is named, and the layout is the pipeline's.
        DescriptorWrites() = default;

        /// For an update of `set`, through `vkUpdateDescriptorSets`.
        explicit DescriptorWrites(VkDescriptorSet set)
            : mSet(set)
        {
        }

        DescriptorWrites(const DescriptorWrites&) = delete;
        DescriptorWrites& operator=(const DescriptorWrites&) = delete;

        void image(std::uint32_t binding, const VkDescriptorImageInfo& info,
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        {
            images(binding, std::span<const VkDescriptorImageInfo>(&info, 1), type);
        }

        /// One binding that is an array of `infos.size()` images.
        void images(std::uint32_t binding, std::span<const VkDescriptorImageInfo> infos, VkDescriptorType type)
        {
            assert(mImageCount + infos.size() <= mImages.size() && "more image infos than there is room for");

            const VkDescriptorImageInfo* const at = mImages.data() + mImageCount;
            for (const VkDescriptorImageInfo& info : infos)
                mImages[mImageCount++] = info;

            append(binding, type, static_cast<std::uint32_t>(infos.size()), nullptr, at, nullptr);
        }

        void buffer(std::uint32_t binding, const VkDescriptorBufferInfo& info,
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
        {
            assert(mBufferCount < mBuffers.size() && "more buffer infos than there is room for");

            mBuffers[mBufferCount] = info;
            append(binding, type, 1, nullptr, nullptr, &mBuffers[mBufferCount++]);
        }

        /// The one write whose payload hangs off `pNext` rather than off a pointer field. `next`
        /// has to outlive the push, as the structure it names does.
        void structure(std::uint32_t binding, const VkWriteDescriptorSetAccelerationStructureKHR& next)
        {
            append(binding, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, &next, nullptr, nullptr);
        }

        std::span<const VkWriteDescriptorSet> get() const
        {
            return std::span<const VkWriteDescriptorSet>(mWrites.data(), mCount);
        }

        std::size_t size() const { return mCount; }

    private:
        void append(std::uint32_t binding, VkDescriptorType type, std::uint32_t count, const void* next,
            const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* block)
        {
            assert(mCount < mWrites.size() && "more descriptor writes than the layout has bindings");
            assert(
                (mCount == 0 || mWrites[mCount - 1].dstBinding < binding) && "descriptor writes out of binding order");

            mWrites[mCount++] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = next,
                .dstSet = mSet,
                .dstBinding = binding,
                .descriptorCount = count,
                .descriptorType = type,
                .pImageInfo = image,
                .pBufferInfo = block,
            };
        }

        VkDescriptorSet mSet = VK_NULL_HANDLE;
        std::array<VkDescriptorImageInfo, Images> mImages{};
        std::array<VkDescriptorBufferInfo, Bindings> mBuffers{};
        std::array<VkWriteDescriptorSet, Bindings> mWrites{};
        std::size_t mImageCount = 0;
        std::size_t mBufferCount = 0;
        std::size_t mCount = 0;
    };

    /// Binds, pushes and launches: the calls every compute pass in this backend ends with. A
    /// pipeline with no bindings pushes nothing, because a push of nought writes is not a push
    /// Vulkan takes.
    template <class Constants>
    void dispatch(VkCommandBuffer commands, const ComputePipeline& pipeline,
        std::span<const VkWriteDescriptorSet> writes, const Constants& constants, std::uint32_t groupsX,
        std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1)
    {
        bind(commands, pipeline);
        if (!writes.empty())
            pushDescriptors(commands, pipeline, writes);
        pushConstants(commands, pipeline, constants);
        vkCmdDispatch(commands, groupsX, groupsY, groupsZ);
    }
}
