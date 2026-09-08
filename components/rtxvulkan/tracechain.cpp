#include "tracechain.hpp"

#include <algorithm>
#include <cassert>

#include "gputimer.hpp"

namespace Rtx
{
    TraceChain::TraceChain(const Device& device, CommandPool& pool, const SetLayout& channels, const SetLayout& fog,
        const std::filesystem::path& shaders, const VkImageUsageFlags colourUsage, const std::string_view colourName)
        : mDevice(device)
        , mPool(pool)
        , mChannelLayout(channels)
        , mFogVolumeLayout(fog)
        , mColourUsage(colourUsage)
        , mColourName(colourName)
        , mAccumulate(device, shaders)
        , mFilter(device, shaders)
    {
    }

    void TraceChain::resize(const std::uint32_t width, const std::uint32_t height)
    {
        assert(width > 0 && height > 0);

        mWidth = width;
        mHeight = height;

        mColour = std::make_unique<Image>(
            mDevice, mWidth, mHeight, VK_FORMAT_R32G32B32A32_SFLOAT, mColourUsage, mColourName);

        mChannels = std::make_unique<GBuffer>(mDevice, mChannelLayout, mWidth, mHeight);
        mFogVolume = std::make_unique<FogVolume>(mDevice, mPool, mFogVolumeLayout, mWidth, mHeight);
        mAccumulate.resize(mWidth, mHeight);
        mFilter.resize(mWidth, mHeight);
    }

    bool TraceChain::grow(const std::uint32_t width, const std::uint32_t height)
    {
        if (isBuilt() && width <= mWidth && height <= mHeight)
            return false;

        resize(std::max(mWidth, width), std::max(mHeight, height));
        return true;
    }

    const Image& TraceChain::recordDenoise(const VkCommandBuffer commands, const Shaders::Camera& camera,
        const float far, const bool historyLost, GpuTimer* const timer)
    {
        // **The temporal half first, and the cascade is what fills in where it was rejected.**
        // The accumulator replaces the trace's single sample with the mean of the frames this
        // surface has been seen over, and hands on the variance of that mean — which is what
        // lets the levels below stop at an edge in the light rather than only at an edge in the
        // geometry.
        openZone(timer, commands, "accumulate");
        const Image& moments = mAccumulate.record(commands, *mChannels, camera, far, historyLost);
        const Image& blended = mAccumulate.getBlended();
        closeZone(timer, commands);

        // The cascade reads what the accumulator just wrote, in both images. The history it
        // writes for the next frame is ordered by the discard `AccumulatePass::record` made of
        // it, which named a compute write as what would come next.
        for (const Image* written : { &blended, &moments })
            written->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        openZone(timer, commands, "filter");
        const Image& indirect
            = mFilter.record(commands, *mChannels, blended, moments, mAccumulate.getHistory(), camera);
        closeZone(timer, commands);

        return indirect;
    }
}
