#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "computepipeline.hpp"
#include "handles.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;

    /// What a lens does with the light that reached it: the frame's own brightness, spread through
    /// Jorge Jimenez's pyramid (*Next Generation Post Processing in Call of Duty: Advanced
    /// Warfare*) — a thirteen-tap halving per level and a nine-tap tent back up, mixed rather than
    /// added, for a third of the frame's pixels and none of a Gaussian stack's banding. No
    /// threshold, because a threshold is a brightness at which the veil switches on. This builds
    /// the pyramid and `TonePass` spreads it, so `FrameImage::Composite` stays the trace's own
    /// answer that a measurement can hand-compute.
    class BloomPass
    {
    public:
        BloomPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Builds a pyramid for a frame this size, if the last one was not this size.
        ///
        /// Idempotent, and the caller is expected to have waited for anything still reading the old
        /// one. Before the first frame.
        void resize(std::uint32_t width, std::uint32_t height);

        /// Builds the pyramid out of `frame`, leaving `frame` as it found it.
        ///
        /// @param frame the finished frame in linear radiance, in `VK_IMAGE_LAYOUT_GENERAL`, at the
        ///        extent `resize` was told, with `VK_IMAGE_USAGE_SAMPLED_BIT`.
        void record(VkCommandBuffer commands, const Image& frame) const;

        /// The finest level, which after `record` holds the blur of every level under it — or null
        /// where the frame was too small to halve.
        ///
        /// Left in `VK_IMAGE_LAYOUT_GENERAL` and already ordered against a sampled read.
        const Image* getPyramid() const { return mLevels.empty() ? nullptr : mLevels.front().get(); }

        /// How many halvings the last `resize` had room for, which is `BLOOM_LEVELS` for any frame
        /// anyone plays at and fewer for the small ones a test and a thumbnail render.
        std::size_t getLevelCount() const { return mLevels.size(); }

    private:
        /// Orders the level just written against the dispatch about to read it.
        void handOver(VkCommandBuffer commands, const Image& level) const;

        /// One dispatch: `source` sampled, `target` written, over `target`'s own extent.
        void run(VkCommandBuffer commands, const ComputePipeline& pipeline, const Image& source, const Image& target,
            float mix) const;

        const Device& mDevice;

        ComputePipeline mHalvePipeline;
        ComputePipeline mSpreadPipeline;

        /// Linear and clamped, which is what both kernels are counted in: every tap sits on a texel
        /// corner so one fetch reads four texels, and a tap that ran off the edge would otherwise
        /// wrap the far side of the frame into the near one's glow.
        Sampler mSampler;

        /// Finest first, each half the one before it. Empty until `resize`.
        std::vector<std::unique_ptr<Image>> mLevels;
    };
}
