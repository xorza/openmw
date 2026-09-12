#include "tracechain.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include <components/rtx/frameimage.hpp>
#include <components/rtx/shaders/composite.h>

#include "compositepass.hpp"
#include "gputimer.hpp"
#include "placing.hpp"
#include "scenebuffers.hpp"
#include "spritepasses.hpp"
#include "tracerecording.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    namespace
    {
        /// Whether this camera has a sea to synthesise.
        ///
        /// **The shader's own test**, so the two cannot disagree: a cell with no water carries a
        /// level of minus infinity, every "how deep" comes out never positive, and nothing samples
        /// the wave tiles — which is what makes not building them for that trace free. Every
        /// interior is such a frame, and the synthesis was a fifth of a millisecond of device time
        /// in each of them.
        bool hasSea(const Shaders::VisibilityConstants& camera)
        {
            return !std::isinf(camera.mWaterLevel);
        }
    }

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

    void TraceChain::resize(const std::uint32_t width, const std::uint32_t height, const bool layers)
    {
        assert(width > 0 && height > 0);

        mWidth = width;
        mHeight = height;

        mColour = std::make_unique<Image>(
            mDevice, mWidth, mHeight, VK_FORMAT_R32G32B32A32_SFLOAT, mColourUsage, mColourName);

        mChannels = std::make_unique<GBuffer>(mDevice, mPool, mChannelLayout, mWidth, mHeight, layers);
        mFogVolume = std::make_unique<FogVolume>(mDevice, mPool, mFogVolumeLayout, mWidth, mHeight);
        mAccumulate.resize(mWidth, mHeight);
        mFilter.resize(mWidth, mHeight);
    }

    void TraceChain::grow(const std::uint32_t width, const std::uint32_t height, const bool layers)
    {
        if (holds(width, height))
            return;

        resize(std::max(mWidth, width), std::max(mHeight, height), layers);
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

        // Both written whole before anything reads them, so neither needs its contents carried over
        // from the last time. **But the last frame may still be reading them** — the curve's output
        // is what the interface draws over and the presenter blits, the colour is what an upscaler
        // and the curve read — so the discard is sourced at everything before it on the queue rather
        // than at the top of the pipe, which would wait for nothing. A picture rides the queue behind
        // the picture before it and is ordered against it by the same scope.
        for (const Image* image : { static_cast<const Image*>(mColour.get()), what.mTarget })
            image->transition(commands,
                ImageUse{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT },
                Use::sComputeWrite);

        // Before the trace and outside its zone, because the sea is a function of the clock and of
        // nothing the camera does — one synthesis serves every ray. None where there is no water:
        // `WavePass::record` says where the tiles are left.
        if (hasSea(what.mSampled))
        {
            openZone(what.mTimer, commands, "waves");
            what.mInputs.mWaves->record(commands, what.mSampled.mTime);
            closeZone(what.mTimer, commands);
        }

        // **The sprite tiles are screen space, so they belong to the camera and not to the scene.**
        // Binned on the device, into the copy this trace is about to read, and ahead of that trace.
        // Not at all for a camera handed a list of its own, which is the one that draws none.
        if (what.mInputs.mSpriteList == 0)
            what.mBuffers->binSprites(*what.mSpriteShade, *what.mSpriteBin, what.mAsked.mOrigin, what.mAsked.mCamera,
                what.mAsked.mSunPosition,
                Placing{
                    .mCommands = commands,
                    .mSlot = what.mInputs.mSlot,
                    .mTimer = what.mTimer,
                    .mGraveyard = *what.mGraveyard,
                });

        mChannels->begin(commands);
        what.mVisibility->record(
            commands, what.mInputs, *mChannels, *what.mCounts, what.mSampled, what.mAirLost, what.mTimer);
        mChannels->handOver(commands);

        // Where the bounce ended up: the filter's last level, or the channel the trace wrote where
        // nothing filtered it.
        const Image* indirect = &mChannels->get(Channel::Indirect);
        if (what.mFilter)
            indirect
                = &recordDenoise(commands, what.mSampled.mCamera, what.mSampled.mFar, what.mHistoryLost, what.mTimer);

        openZone(what.mTimer, commands, "composite");
        what.mComposite->record(commands, *mChannels, *indirect, what.mSum, *mColour,
            Shaders::CompositeConstants{
                .mWidth = what.mSampled.mCamera.mWidth,
                .mHeight = what.mSampled.mCamera.mHeight,
                .mAccumulate = what.mAccumulate,
            });
        closeZone(what.mTimer, commands);

        // Whatever comes next reads what the composite just wrote. The frame's scope is the wider of
        // the two — an upscaler, a lens and a curve against a picture's one curve — and covers both.
        mColour->transition(commands, Use::sComputeWrite, Use::sAnyGeneralRead);

        return *mColour;
    }
}
