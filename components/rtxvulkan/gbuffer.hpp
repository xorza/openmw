#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/channel.hpp>
#include <components/rtx/shaders/gbuffer.h>

#include "image.hpp"
#include "owned.hpp"
#include "setlayout.hpp"

namespace Rtx
{
    class Device;

    /// What the trace leaves behind, before anything has decided what the picture looks like.
    ///
    /// **A picture cannot be filtered and these can.** One bounce per pixel is noisy, and the only
    /// thing that removes noise without removing detail is a blur that runs over the light alone —
    /// which means the light has to still be separate from the surface it landed on when the blur
    /// reaches it. By the time a pixel is a colour, the albedo has been multiplied in, the fog has
    /// been laid over it and the curve has been applied; there is nothing left to filter that would
    /// not also smear the wall's texture.
    ///
    /// So the trace writes what it knows in the form the next pass can use, and the composite puts
    /// it back together:
    ///
    ///     colour = direct + albedo * filter(indirect * transmittance)
    ///
    /// **And this is the same buffer Ray Reconstruction reads.** It asks for exactly this —
    /// demodulated radiance, the albedo to put back, normals and depth — so the split earns its
    /// place twice over even if the filter written on top of it is later replaced.
    class GBuffer
    {
    public:
        /// @param layers whether anything after this chain reads the layer the eye sees through.
        ///        Only an upscaled frame does — `VisibilityConstants::mLayerCompositedAfter` is the
        ///        same question asked of the shader — and where nothing does, the three channels
        ///        are one texel each instead of the frame's own extent.
        GBuffer(const Device& device, CommandPool& pool, const SetLayout& layout, std::uint32_t width,
            std::uint32_t height, bool layers);

        /// The set every `GBuffer` is addressed through, made once and outliving all of them.
        ///
        /// **Separate from the buffer because a pipeline layout names every set it will ever be
        /// handed**, and the trace's pipelines are built before any camera has a size.
        static SetLayout describeLayout(const Device& device);

        GBuffer(const GBuffer&) = delete;
        GBuffer& operator=(const GBuffer&) = delete;

        /// One channel's image, which is the image bound at that channel's number.
        const Image& get(Channel channel) const { return mChannels[bindingOf(channel)]; }

        /// Whether a channel is the frame's own extent rather than a stand-in nothing reads.
        bool carries(Channel channel) const { return bindingOf(channel) < mCarried; }

        VkDescriptorSet getSet() const { return mSet; }

        std::uint32_t getWidth() const { return get(Channel::Direct).getWidth(); }
        std::uint32_t getHeight() const { return get(Channel::Direct).getHeight(); }

        /// Discards the contents and makes every channel writable, which is how a frame starts.
        ///
        /// Waits for the previous frame's composite to have read them, so that one set of channels
        /// can serve a window that keeps several frames in flight.
        void begin(VkCommandBuffer commands) const;

        /// Orders the pass that wrote them against the pass about to read them, or in the
        /// accumulator's case to write one of them back.
        void handOver(VkCommandBuffer commands) const;

    private:
        /// **An array and not fourteen members.** Named three times each — a member, an accessor,
        /// and a hand-written table mapping the binding back — a channel added to `channel.hpp`
        /// without the third reaches its pass as a null.
        std::vector<Image> mChannels;

        /// How many of them are the frame's own extent, which is all of them or all but the three
        /// layer channels. They are last, so one count says which.
        std::uint32_t mCarried;

        /// The set goes with the pool it came out of, which is what one pool per buffer is for.
        Owned<VkDescriptorPool, vkDestroyDescriptorPool> mPool;
        VkDescriptorSet mSet = VK_NULL_HANDLE;
    };
}
