#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/sets.h>
#include <components/rtx/texturewrap.hpp>

#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// The handles this renderer makes in one place and holds in many, each as its `Owned`: the
    /// type already says what it owns and when it ends, so what is left to say is how one is made.
    using ShaderModule = Owned<VkShaderModule, vkDestroyShaderModule>;
    using SetLayout = Owned<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout>;
    using Sampler = Owned<VkSampler, vkDestroySampler>;
    using Semaphore = Owned<VkSemaphore, vkDestroySemaphore>;
    using Fence = Owned<VkFence, vkDestroyFence>;
    using QueryPool = Owned<VkQueryPool, vkDestroyQueryPool>;

    /// A binary semaphore, for what a present hands back and forth: the timeline is the queue's
    /// one clock and a swapchain cannot read it.
    Semaphore makeSemaphore(const Device& device);

    /// The timeline semaphore the queue's clock is, starting at nought.
    Semaphore makeTimelineSemaphore(const Device& device, std::string_view name);

    /// A fence that starts signalled, so the first wait on it returns at once.
    Fence makeSignalledFence(const Device& device);

    /// A `VkShaderModule` built from a SPIR-V file the build produced and validated; what this
    /// checks is that the file is the one the build wrote, because a truncated `.spv` is otherwise
    /// a driver crash with no explanation.
    ShaderModule loadShaderModule(const Device& device, const std::filesystem::path& path);

    /// A descriptor set layout, for `GBuffer::describeLayout` and its siblings to build theirs
    /// through. `flags` is what a push descriptor set needs; `next` is binding flags for a bindless
    /// set, read here and never kept.
    SetLayout makeSetLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        VkDescriptorSetLayoutCreateFlags flags = 0, const void* next = nullptr);

    /// The two shapes this renderer reads images through, both linear over the whole chain, because
    /// written out per class the fields drift where nothing decides. Targets clamp to the edge,
    /// because a bloom level or a volume slice runs to the edge of what it was given; content
    /// addresses its edges as the file said — repeating for what tiles, which is nearly everything,
    /// and clamping along whichever axis a banner's or a flame's sheet stops at.
    Sampler makeTargetSampler(const Device& device, std::string_view name);
    Sampler makeContentSampler(const Device& device, std::string_view name, TextureWrap wrap = TextureWrap::Repeat);

    /// Linear over the whole chain and nothing past the edge: a field that ends in still water
    /// reads as still water beyond it.
    Sampler makeBorderSampler(const Device& device, std::string_view name);

    // The pushed set first and the shared ones after it with no gap, because Vulkan wants a layout
    // at every number below the highest a pipeline names and a set a pass does not read has none.
    static_assert(Shaders::SET_PASS == 0, "the pass's own set is the first");

    /// The sets a pipeline reads beside its own pushed one, each named for what it holds. A layout
    /// and a bind put each at the number `shaders/sets.h` gives it, so no list's order has to agree
    /// with another's. Null is a set the pipeline does not read.
    template <class Handle>
    struct SharedSets
    {
        Handle mTextures = VK_NULL_HANDLE;
        Handle mChannels = VK_NULL_HANDLE;
        Handle mVolume = VK_NULL_HANDLE;

        /// Every set at its number, with null at `SET_PASS`, which is the pipeline's own.
        std::array<Handle, Shaders::SET_COUNT> byNumber() const
        {
            std::array<Handle, Shaders::SET_COUNT> sets{};
            sets[Shaders::SET_TEXTURES] = mTextures;
            sets[Shaders::SET_CHANNELS] = mChannels;
            sets[Shaders::SET_VOLUME] = mVolume;
            return sets;
        }
    };

    using SharedSetLayouts = SharedSets<VkDescriptorSetLayout>;
    using SharedSetBinds = SharedSets<VkDescriptorSet>;

    /// A pass's own descriptor set layout and the pipeline layout that names it and the shared sets
    /// — one statement for compute, trace and graphics pipelines, which differ in nothing about how
    /// descriptors reach them. `SET_PASS` is always a push descriptor set: nothing in this renderer
    /// wants a descriptor pool on the frame path.
    class PipelineLayout
    {
    public:
        /// Nothing passed outlives the call. `bindings` is `SET_PASS`; `push` is the one push range,
        /// whole at offset zero, and a size of nought declares none, because Vulkan takes no empty
        /// range and a pass whose constants moved into a buffer asks for exactly that; `shared` is
        /// every other set the layout will ever be handed.
        PipelineLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            const VkPushConstantRange& push, const SharedSetLayouts& shared);

        VkPipelineLayout getHandle() const { return mHandle.get(); }

        /// The range as declared, so a push is written with the stages the layout named and checked
        /// against the size it named.
        const VkPushConstantRange& getPushRange() const { return mPush; }

        /// How many sets the layout names, its own among them: the shared ones are the numbers
        /// between `SET_PASS` and this, which is what a bind has to hand over.
        std::uint32_t getSetCount() const { return mSetCount; }

    private:
        SetLayout mSetLayout;
        Owned<VkPipelineLayout, vkDestroyPipelineLayout> mHandle;
        VkPushConstantRange mPush;
        std::uint32_t mSetCount = 0;
    };
}
