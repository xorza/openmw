#include "accumulatehistory.hpp"

#include <cassert>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/accumulate.h>
#include <components/rtx/shaders/atrous.h>

namespace Rtx
{
    namespace
    {
        /// `SAMPLED` beside `STORAGE` on what the cascade after the accumulator reads. `AtrousPass`
        /// takes its taps through the texture unit and a sampled descriptor needs the bit at
        /// creation, which is a promise made here and kept there.
        constexpr VkImageUsageFlags sReadAndWrite = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    AccumulateHistory::AccumulateHistory(const Device& device)
        : mDevice(device)
    {
    }

    void AccumulateHistory::resize(std::uint32_t width, std::uint32_t height)
    {
        if (!mBlended.isEmpty() && mBlended.getWidth() == width && mBlended.getHeight() == height)
            return;

        for (std::size_t i = 0; i < 2; ++i)
        {
            mColour[i] = Image(mDevice, width, height, ACCUMULATE_COLOUR, sReadAndWrite,
                i == 0 ? "accumulate-colour-0" : "accumulate-colour-1");
            mSurface[i] = Image(mDevice, width, height, ACCUMULATE_SURFACE, VK_IMAGE_USAGE_STORAGE_BIT,
                i == 0 ? "accumulate-surface-0" : "accumulate-surface-1");
            mMoments[i] = Image(mDevice, width, height, ACCUMULATE_MOMENTS, sReadAndWrite,
                i == 0 ? "accumulate-moments-0" : "accumulate-moments-1");
        }

        mBlended = Image(mDevice, width, height, ATROUS_CHANNEL, sReadAndWrite, "accumulate-blended");

        mCurrent = 0;
        mFresh = true;
    }

    AccumulateHistory::Turn AccumulateHistory::turn()
    {
        assert(!mBlended.isEmpty() && "a turn before resize");

        const std::size_t previous = mCurrent;
        mCurrent = 1 - mCurrent;

        const bool fresh = mFresh;
        mFresh = false;

        return Turn{
            .mColourBefore = mColour[previous],
            .mSurfaceBefore = mSurface[previous],
            .mMomentsBefore = mMoments[previous],
            .mColour = mColour[mCurrent],
            .mSurface = mSurface[mCurrent],
            .mMoments = mMoments[mCurrent],
            .mBlended = mBlended,
            .mFresh = fresh,
        };
    }

    const Image& AccumulateHistory::getBlended() const
    {
        assert(!mBlended.isEmpty() && "the blend asked for before a resize made one");
        assert(!mFresh && "the blend asked for before any frame was accumulated into it");

        return mBlended;
    }

    const Image& AccumulateHistory::getHistory() const
    {
        assert(!mColour[mCurrent].isEmpty() && "the history asked for before a resize made one");
        assert(!mFresh && "the history asked for before the frame that hands it over");

        return mColour[mCurrent];
    }
}
