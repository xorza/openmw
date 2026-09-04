#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "image.hpp"
#include "setlayout.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// Filtered coverage and lighting; the pixel shader integrates the analytic medium along its own ray.
    class FogVolume
    {
    public:
        FogVolume(const Device& device, CommandPool& pool, const SetLayout& layout, std::uint32_t width,
            std::uint32_t height);

        static SetLayout describeLayout(const Device& device);
        ~FogVolume();

        FogVolume(const FogVolume&) = delete;
        FogVolume& operator=(const FogVolume&) = delete;

        std::uint32_t getColumns() const { return mColumns; }
        std::uint32_t getRows() const { return mRows; }

        VkDescriptorSet getSet(std::uint64_t frame) const { return mSets[writtenAt(frame)]; }

        void begin(VkCommandBuffer commands, std::uint64_t frame) const;
        void depthTaken(VkCommandBuffer commands) const;
        void scattered(VkCommandBuffer commands, std::uint64_t frame) const;
        void handOver(VkCommandBuffer commands) const;

    private:
        static std::size_t writtenAt(std::uint64_t frame) { return frame & 1; }

        void destroy();

        const Device& mDevice;

        std::uint32_t mColumns = 0;
        std::uint32_t mRows = 0;

        // Only sampled coverage and visibility are reprojected; deterministic transport never enters history.
        std::array<Image, 2> mCoverage;
        std::array<Image, 2> mVisibility;

        // Lamp energy is integrated along each froxel and follows flicker without temporal averaging.
        Image mLamps;

        Image mSlice;
        Image mSliceVisibility;
        Image mColumnDepth;

        VkSampler mSampler = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, 2> mSets{};
    };
}
