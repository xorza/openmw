#include "tracechain.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include <components/rtx/frameimage.hpp>
#include <components/rtx/shaders/composite.h>
#include <components/rtx/shaders/visibility.h>

#include "barriers.hpp"
#include "compositepass.hpp"
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
    namespace
    {
        /// Whether this camera has a sea to synthesise — the shader's own test, so the two cannot
        /// disagree. Every interior is such a frame, and the synthesis was a fifth of a millisecond
        /// of device time in each of them.
        bool hasSea(const Shaders::VisibilityConstants& camera)
        {
            return !std::isinf(camera.mWaterLevel);
        }
    }

    TraceChain::TraceChain(const Device& device, Graveyard& graveyard, CommandPool& pool, const SetLayout& channels,
        const SetLayout& fog, const std::filesystem::path& shaders, const VkImageUsageFlags colourUsage,
        const std::string_view colourName)
        : mDevice(device)
        , mPool(pool)
        , mChannelLayout(channels)
        , mFogVolumeLayout(fog)
        , mColourUsage(colourUsage)
        , mColourName(colourName)
        , mBins([&](FrameSlot) {
            return SpriteBin{ device, graveyard };
        })
        , mAccumulate(device, shaders)
        , mFilter(device, shaders)
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

        mChannels = std::make_unique<GBuffer>(mDevice, mChannelLayout, mWidth, mHeight, radiance);
        mFogVolume = std::make_unique<FogVolume>(mDevice, mPool, mFogVolumeLayout, mWidth, mHeight);
        mAccumulate.resize(mWidth, mHeight);
        mFilter.resize(mWidth, mHeight);
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
        const Image& moments = mAccumulate.record(commands, *mChannels, camera, far, historyLost);
        const Image& blended = mAccumulate.getBlended();
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
        const Image& indirect
            = mFilter.record(commands, *mChannels, blended, moments, mAccumulate.getHistory(), camera);
        closeZone(timer, commands);

        return indirect;
    }

    const Image& TraceChain::record(const VkCommandBuffer commands, const TraceRecording& what)
    {
        assert(isBuilt() && "a trace into a chain that has no extent");

        // Both written whole before anything reads them. The last frame may still be reading
        // them, and the head barrier `CommandPool::begin` recorded is what orders this buffer after
        // it, so the discard itself waits for nothing.
        for (const Image* image : { static_cast<const Image*>(&mColour), what.mTarget })
            image->transition(commands, Use::sUndefined, Use::sComputeWrite);

        // Before the trace and outside its zone, because the sea is a function of the clock and of
        // nothing the camera does — one synthesis serves every ray. None where there is no water:
        // `WavePass::record` says where the tiles are left.
        if (hasSea(what.mSampled))
        {
            openZone(what.mTimer, commands, "waves");
            what.mInputs.mWaves->record(commands, what.mSampled.mTime);
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
        SpriteBin& bin = mBins.at(what.mBinSlot);
        VisibilityInputs inputs = what.mInputs;
        inputs.mBin = &bin;
        const bool bins = inputs.mSpriteList == 0;
        const SpriteSource sprites = what.mBuffers->describeSprites(inputs.mSlot);
        if (bins)
            bin.take(sprites, what.mAsked.mCamera, commands);

        what.mVisibility->writeFrame(commands, inputs, what.mSampled, what.mAirLost);

        if (bins)
        {
            what.mVisibility->recordSpriteShelter(
                commands, inputs, *mChannels, *what.mCounts, what.mSampled, sprites.mSpriteCount, what.mTimer);
            bin.record(*what.mSpriteShade, *what.mSpriteBin, sprites, what.mAsked.mOrigin, what.mAsked.mCamera,
                what.mAsked.mSun.mDirection, commands, what.mTimer);
        }

        mChannels->begin(commands);
        what.mVisibility->record(commands, inputs, *mChannels, *what.mCounts, what.mSampled, what.mTimer);
        mChannels->handOver(commands);

        // Where the bounce ended up: the filter's last level, or the channel the trace wrote where
        // nothing filtered it.
        const Image* indirect = &mChannels->get(Channel::Indirect);
        if (what.mFilter)
            indirect
                = &recordDenoise(commands, what.mSampled.mCamera, what.mSampled.mFar, what.mHistoryLost, what.mTimer);

        openZone(what.mTimer, commands, "composite");
        what.mComposite->record(commands, *mChannels, *indirect, what.mSum, mColour,
            Shaders::CompositeConstants{
                .mWidth = what.mSampled.mCamera.mWidth,
                .mHeight = what.mSampled.mCamera.mHeight,
                .mAccumulate = what.mAccumulate,
            });
        closeZone(what.mTimer, commands);

        // Whatever comes next reads what the composite just wrote. The frame's scope is the wider of
        // the two — an upscaler, a lens and a curve against a picture's one curve — and covers both.
        mColour.transition(commands, Use::sComputeWrite, Use::sAnyGeneralRead);

        return mColour;
    }
}
