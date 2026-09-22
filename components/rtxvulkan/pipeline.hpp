#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <utility>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/sets.h>

#include "handles.hpp"
#include "owned.hpp"

namespace Rtx
{
    /// What the three kinds of pipeline share: the handle, the layout descriptors are pushed
    /// against and push constants written through, and the bind point the two are addressed at.
    /// One object with its layout because they fail as one: a constructor that throws gets no
    /// destructor, so a pass that made these itself left a layout behind for `vkDestroyDevice` to
    /// find.
    class Pipeline
    {
    public:
        VkPipeline getHandle() const { return mHandle.get(); }
        VkPipelineLayout getLayout() const { return mLayout.getHandle(); }
        VkPipelineBindPoint getBindPoint() const { return mBindPoint; }
        const VkPushConstantRange& getPushRange() const { return mLayout.getPushRange(); }
        std::uint32_t getSetCount() const { return mLayout.getSetCount(); }

    protected:
        Pipeline(PipelineLayout&& layout, VkPipelineBindPoint bindPoint)
            : mLayout(std::move(layout))
            , mBindPoint(bindPoint)
        {
        }

        PipelineLayout mLayout;
        Owned<VkPipeline, vkDestroyPipeline> mHandle;

    private:
        VkPipelineBindPoint mBindPoint;
    };

    inline void bind(VkCommandBuffer commands, const Pipeline& pipeline)
    {
        vkCmdBindPipeline(commands, pipeline.getBindPoint(), pipeline.getHandle());
    }

    /// Pushes `SET_PASS`, which every pipeline here declares as a push descriptor set.
    inline void pushDescriptors(
        VkCommandBuffer commands, const Pipeline& pipeline, std::span<const VkWriteDescriptorSet> writes)
    {
        vkCmdPushDescriptorSet(commands, pipeline.getBindPoint(), pipeline.getLayout(), Shaders::SET_PASS,
            static_cast<std::uint32_t>(writes.size()), writes.data());
    }

    /// Binds every shared set the layout names, each at its own number: a layout is handed exactly
    /// the sets it was made with, and a bind short of one is a validation error at best.
    inline void bindSets(VkCommandBuffer commands, const Pipeline& pipeline, const SharedSetBinds& shared)
    {
        constexpr std::uint32_t first = Shaders::SET_PASS + 1;
        const std::array<VkDescriptorSet, Shaders::SET_COUNT> sets = shared.byNumber();
        const std::uint32_t count = pipeline.getSetCount();
        assert(count > first && "a bind for a pipeline that reads no shared set");
        for (std::uint32_t set = first; set < Shaders::SET_COUNT; ++set)
            assert((sets[set] != VK_NULL_HANDLE) == (set < count) && "a bind of other sets than the layout names");

        vkCmdBindDescriptorSets(commands, pipeline.getBindPoint(), pipeline.getLayout(), first, count - first,
            sets.data() + first, 0, nullptr);
    }

    /// Writes the whole push range, to the stages the layout declared it for. The size is the
    /// layout's, so a struct that grew on one side and not the other is caught here rather than
    /// read as garbage past the end of what was pushed. Copied by Vulkan before this returns.
    template <class Constants>
    void pushConstants(VkCommandBuffer commands, const Pipeline& pipeline, const Constants& constants)
    {
        const VkPushConstantRange& range = pipeline.getPushRange();
        assert(sizeof(Constants) == range.size && "push constants of a size the layout did not declare");

        vkCmdPushConstants(commands, pipeline.getLayout(), range.stageFlags, 0, range.size, &constants);
    }
}
