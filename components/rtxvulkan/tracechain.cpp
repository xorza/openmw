#include "tracechain.hpp"

#include <algorithm>
#include <cassert>

#include <components/rtx/frameimage.hpp>
#include <components/rtx/shaders/composite.h>
#include <components/rtx/shaders/visibility.h>

#include "accumulatepass.hpp"
#include "atrouspass.hpp"
#include "barriers.hpp"
#include "compositepass.hpp"
#include "formats.hpp"
#include "gputimer.hpp"
#include "handles.hpp"
#include "imageuse.hpp"
#include "scenebuffers.hpp"
#include "spritepasses.hpp"
#include "tracerecording.hpp"
#include "visibilitypass.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    TraceChain::TraceChain(const Device& device, const TracePasses& passes, const VkImageUsageFlags colourUsage,
        const std::string_view colourName)
        : mDevice(device)
        , mPasses(passes)
        , mColourUsage(colourUsage)
        , mColourName(colourName)
        , mBins([&](FrameSlot) { return SpriteBin{ device }; })
        , mHistory(device)
    {
    }

    void TraceChain::resize(const std::uint32_t width, const std::uint32_t height, const RadianceWidth radiance)
    {
        assert(width > 0 && height > 0);

        mWidth = width;
        mHeight = height;

        // As wide as the channels it is composed from: what is summed is summed out of this image,
        // and what is shown is shown from it.
        mColour = Image(mDevice, mWidth, mHeight, radianceFormat(radiance), mColourUsage, mColourName);

        mChannels = std::make_unique<GBuffer>(mDevice, mPasses.mChannels, mWidth, mHeight, radiance);
        mFogVolume = std::make_unique<FogVolume>(mDevice, mPasses.mFog, mWidth, mHeight);
        mHistory.resize(mWidth, mHeight);
        if (mFilterScratch.isEmpty() || mFilterScratch.getWidth() != mWidth || mFilterScratch.getHeight() != mHeight)
            mFilterScratch = AtrousPass::makeScratch(mDevice, mWidth, mHeight);

        // Dropped rather than resized, because most runs never make one: sixteen bytes a pixel is
        // worth it to the reference mode and nothing to a window. The first averaging trace asks.
        dropSum();
    }

    void TraceChain::grow(const std::uint32_t width, const std::uint32_t height, const RadianceWidth radiance)
    {
        if (holds(width, height))
            return;

        resize(std::max(mWidth, width), std::max(mHeight, height), radiance);
    }

    const Image& TraceChain::recordDenoise(const VkCommandBuffer commands, const Shaders::Camera& camera,
        const float far, const bool historyLost, GpuTimer* const timer)
    {
        // The temporal half first: the accumulator hands on the variance of its mean, which is
        // what lets the levels below stop at an edge in the light and not only in the geometry.
        openZone(timer, commands, "accumulate");
        const Image& moments = mPasses.mAccumulate.record(commands, mHistory, *mChannels, camera, far, historyLost);
        const Image& blended = mHistory.getBlended();
        closeZone(timer, commands);

        // The cascade reads what the accumulator just wrote, in both images, and it reads through
        // the texture unit — so the dependency names the sampled access and not only the storage
        // one. The history the cascade writes for the next frame is ordered by the discard
        // `AccumulatePass::record` made of it, which named a compute write as what would come next.
        Barriers handed(commands);
        for (const Image* written : { &blended, &moments })
            handed.add(written->describeTransition(Use::sComputeWrite, Use::sComputeReadOrSample));

        handed.flush();

        openZone(timer, commands, "filter");
        const Image& indirect = mPasses.mFilter.record(
            commands, *mChannels, blended, moments, mHistory.getHistory(), mFilterScratch, camera);
        closeZone(timer, commands);

        return indirect;
    }

    void TraceChain::resetHistory()
    {
        mHistory.reset();
        mAirStale = true;
    }

    VkDeviceAddress TraceChain::getSpriteTileList(const VisibilityInputs& inputs) const
    {
        return inputs.mSpriteList != 0 ? inputs.mSpriteList : mBins.at(inputs.mTraceSlot).getTileListAddress();
    }

    TraceResult TraceChain::record(const VkCommandBuffer commands, const TraceRecording& what)
    {
        assert(isBuilt() && "a trace into a chain that has no extent");

        VisibilityInputs inputs = what.mInputs;
        inputs.mChannels = mChannels.get();
        inputs.mFogVolume = mFogVolume.get();

        // Made by the first trace that averages, and that trace is the one that fills it: the first
        // write needs no contents and nothing to wait on, and every trace after reads what the last
        // left, which the head barrier `CommandPool::begin` recorded orders and makes visible.
        if (what.mAccumulate > 0 && mSum.isEmpty())
        {
            mSum = Image(
                mDevice, mWidth, mHeight, toVulkanFormat(COMPOSITE_SUM_FORMAT), VK_IMAGE_USAGE_STORAGE_BIT, "sum");
            mSum.transition(commands, Use::sUndefined, Use::sComputeReadWrite);
        }

        // Both written whole before anything reads them. The last frame may still be reading
        // them, and the head barrier `CommandPool::begin` recorded is what orders this buffer after
        // it, so the discard itself waits for nothing.
        for (const Image* image : { static_cast<const Image*>(&mColour), what.mTarget })
            image->transition(commands, Use::sUndefined, Use::sComputeWrite);

        // Before the trace and outside its zone, because the sea is a function of the clock and of
        // nothing the camera does — one synthesis serves every ray. None where there is no sea,
        // which is most interiors, and a fifth of a millisecond of device time in each of them.
        if (inputs.mSea)
        {
            openZone(what.mTimer, commands, "waves");
            inputs.mWaves->record(commands, what.mSampled.mWaterTime);
            closeZone(what.mTimer, commands);
        }

        // The sprite tiles are screen space, so they belong to the camera and not to the scene.
        // Binned on the device into this trace's own bin, ahead of the trace that reads it. Not at
        // all for a camera handed a list of its own, which is the one that draws none.
        //
        // **Taken, then the block, then the shelter, then the bin.** The block carries the bin's
        // table by address, which it has once the table is taken; the shelter launch reads the
        // block and zeroes the drops under a roof in that table; and the shade and the bin read
        // what is left. Every launch after reads the same block.
        SpriteBin& bin = mBins.at(inputs.mTraceSlot);
        const bool bins = inputs.mSpriteList == 0;
        const SpriteSource sprites = inputs.mBuffers->describeSprites(inputs.mSlot);
        if (bins)
            bin.take(sprites, what.mAsked.mCamera, commands);

        mPasses.mVisibility.writeFrame(
            commands, inputs, bin, getSpriteTileList(inputs), what.mSampled, what.mPastLost || mAirStale);
        mAirStale = false;

        if (bins)
        {
            mPasses.mVisibility.recordSpriteShelter(commands, inputs, what.mSampled, sprites.mSpriteCount, what.mTimer);
            bin.record(commands,
                Binning{
                    .mShading = mPasses.mSpriteShade,
                    .mPass = mPasses.mSpriteBin,
                    .mSource = sprites,
                    .mOrigin = what.mAsked.mOrigin,
                    .mCamera = what.mAsked.mCamera,
                    .mToSun = what.mAsked.mSun.mDirection,
                    .mTimer = what.mTimer,
                });
        }

        mChannels->begin(commands);
        mPasses.mVisibility.record(commands, inputs, what.mSampled, what.mTimer);
        mChannels->handOver(commands);

        // Where the bounce ended up: the filter's last level, or the channel the trace wrote where
        // nothing filtered it.
        const Image* indirect = &mChannels->get(Channel::Indirect);
        if (what.mFilter)
            indirect = &recordDenoise(commands, what.mSampled.mCamera, what.mSampled.mFar, what.mPastLost, what.mTimer);

        openZone(what.mTimer, commands, "composite");
        mPasses.mComposite.record(commands, *mChannels, *indirect, mSum.isEmpty() ? nullptr : &mSum, mColour,
            Shaders::CompositeConstants{
                .mWidth = what.mSampled.mCamera.mWidth,
                .mHeight = what.mSampled.mCamera.mHeight,
                .mAccumulate = what.mAccumulate,
            });
        closeZone(what.mTimer, commands);

        // Whatever comes next reads what the composite just wrote. The frame's scope is the wider of
        // the two — an upscaler, a lens and a curve against a picture's one curve — and covers both.
        mColour.transition(commands, Use::sComputeWrite, Use::sAnyGeneralRead);

        return TraceResult{ .mInputs = inputs, .mColour = mColour, .mSpriteTileList = getSpriteTileList(inputs) };
    }
}
