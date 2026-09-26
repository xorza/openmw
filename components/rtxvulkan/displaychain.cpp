#include "displaychain.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <variant>

#include <components/rtx/shaders/camera.h>
#include <components/rtx/shaders/gbuffer.h>
#include <components/rtx/shaders/line.h>
#include <components/rtx/shaders/tone.h>

#include "buffer.hpp"
#include "gbuffer.hpp"
#include "gputimer.hpp"
#include "image.hpp"
#include "imageuse.hpp"
#include "tracerecording.hpp"
#include "visibilitypass.hpp"

namespace Rtx
{
    namespace
    {
        /// The display pass's own description of the frame, on the picture's grid. The alpha
        /// carries the puffs' transmittance wherever it is not the picture's coverage, which is
        /// the same test `spritecomposite.rgen` makes — and where that composite drew nothing, the
        /// pass finds out for itself from what it is handed here.
        Shaders::ToneConstants toneFor(const Shaders::VisibilityConstants& frame, const SunGlare& fader,
            const VkDeviceAddress spriteTileList, const VkDeviceAddress spritePresence,
            const VkDeviceAddress textureTexels, std::uint32_t width, std::uint32_t height, std::uint32_t tracedWidth,
            std::uint32_t tracedHeight)
        {
            assert(spriteTileList != 0 && spritePresence != 0 && "a curve told no tiles to test the puffs by");
            assert(textureTexels != 0 && "a curve told no texel counts to test the star sheet by");

            return Shaders::ToneConstants{
                .mSpriteTileList = spriteTileList,
                .mSpritePresence = spritePresence,
                .mTextureTexels = textureTexels,
                .mTracedWidth = tracedWidth,
                .mTracedHeight = tracedHeight,
                .mCoverAlpha = frame.mTransparentBackground == 0 ? 1u : 0u,
                .mCamera = Shaders::cameraOnGrid(frame.mCamera, width, height),
                .mStars = frame.mStars,
                .mGlareColour = fader.mColour,
                .mGlareAmount = fader.amountFor(frame),
            };
        }
    }

    DisplayChain::DisplayChain(const Device& device, const VisibilityPass& puffs,
        const VkDescriptorSetLayout textureLayout, const std::filesystem::path& shaders, const VkFormat targetFormat)
        : mDevice(device)
        , mPuffs(puffs)
        , mBloom(device, shaders)
        , mExposure(device, shaders)
        , mSunGlare(device, shaders)
        , mTone(device, textureLayout, shaders)
        , mLines(device, shaders, targetFormat)
    {
    }

    void DisplayChain::resize(const std::uint32_t width, const std::uint32_t height)
    {
        mBloom.resize(width, height);
    }

    void DisplayChain::beginGlare(const VkCommandBuffer commands) const
    {
        mSunGlare.begin(commands);
    }

    void DisplayChain::record(const VkCommandBuffer commands, const Display& what)
    {
        const VisibilityInputs& inputs = what.mTrace.mInputs;
        const Image& shown = *inputs.mShown;
        assert(shown.getWidth() >= what.mExtent.width && shown.getHeight() >= what.mExtent.height);

        // The puffs over the picture, at its own extent, and then the picture is what the lens
        // spreads and the curve maps. The bloom samples what this leaves, rather than loading it —
        // `BloomPass` binds the frame as a combined image sampler — so the scope after it names
        // both reads.
        const GBuffer& channels = *inputs.mChannels;

        mPuffs.recordSpriteComposite(commands, inputs, what.mExtent,
            VkExtent2D{ what.mSampled.mCamera.mWidth, what.mSampled.mCamera.mHeight }, what.mTimer);
        shown.transition(commands, Use::sTraceReadWrite, Use::sComputeReadOrSample);

        // What the lens will spread, built here and applied by the curve. Nothing is written back
        // over the frame — `BloomPass` says why the trace's own answer has to reach `readComposite`
        // untouched.
        if (what.mBloom)
        {
            openZone(what.mTimer, commands, "bloom");
            mBloom.record(commands, shown);
            closeZone(what.mTimer, commands);
        }

        // Measured off the image the curve is about to map, which is the upscaled one wherever
        // something upscales — see `histogram.comp` for what measuring the other one costs. One
        // `mShown` feeds both, so the two cannot come apart. A picture is measured off nothing, or
        // the same armour would be a different brightness in two windows.
        const Buffer* exposure = &mExposure.getPictureExposure();
        if (!std::holds_alternative<Display::Picture>(what.mExposure))
        {
            openZone(what.mTimer, commands, "exposure");
            if (const auto* fixed = std::get_if<Display::Fixed>(&what.mExposure); fixed != nullptr)
                mExposure.recordFixed(commands, fixed->mValue);
            else
            {
                const Display::Measured& measured = std::get<Display::Measured>(what.mExposure);
                mExposure.record(commands, shown, measured.mSeconds, measured.mReset || mExposureStale, measured.mBias);
                mExposureStale = false;
            }
            closeZone(what.mTimer, commands);
            exposure = &mExposure.getExposure();
        }

        // What the eye saw of the sun's quad, eased at the query's own rate, which the curve lays
        // the glare fader over the picture by. Read after the trace and before the curve, on the
        // device: a frame's own count is a frame's own wash.
        const Buffer* share = &mSunGlare.getNoShare();
        if (what.mGlare.has_value())
        {
            openZone(what.mTimer, commands, "glare");
            mSunGlare.record(commands, what.mGlare->mSeconds, what.mGlare->mReset || mGlareStale);
            mGlareStale = false;
            closeZone(what.mTimer, commands);
            share = &mSunGlare.getShare();
        }

        openZone(what.mTimer, commands, "tone");
        mTone.record(commands,
            Tone{
                .mColour = shown,
                .mExposure = *exposure,
                .mSunGlare = *share,
                .mStarsShown = channels.get(Channel::StarsShown),
                .mPuffsDepth = channels.get(Channel::PuffsDepth),
                .mBloom = what.mBloom ? mBloom.getPyramid() : nullptr,
                .mTextures = inputs.mTextures,
                .mTarget = what.mTarget,
                .mConstants = toneFor(what.mSampled, what.mGlare.has_value() ? what.mGlare->mFader : SunGlare{},
                    what.mTrace.mSpriteTileList, what.mTrace.mSpritePresence, inputs.mTextureTexels, what.mExtent.width,
                    what.mExtent.height, channels.getWidth(), channels.getHeight()),
            });
        closeZone(what.mTimer, commands);

        recordDebugLines(commands, what);
    }

    void DisplayChain::recordDebugLines(const VkCommandBuffer commands, const Display& what)
    {
        const DebugLines& debug = what.mDebug;
        if (debug.empty())
            return;

        assert(what.mDebugVertices != nullptr && "lines to draw and no buffer to draw them from");
        const GBuffer& channels = *what.mTrace.mInputs.mChannels;

        // The lines first and the triangles after them, in the slot's own buffer: the frame
        // behind read its own slot's, so nothing here is written under a submit.
        const std::size_t count = debug.mLines.size() + debug.mTriangles.size();
        outgrow(*what.mDebugVertices, mDevice, BufferKind::HostWritten, count * sizeof(DebugVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "debug vertices");

        const std::span<DebugVertex> written = what.mDebugVertices->writable<DebugVertex>(0, count);
        std::copy(debug.mLines.begin(), debug.mLines.end(), written.begin());
        std::copy(debug.mTriangles.begin(), debug.mTriangles.end(), written.begin() + debug.mLines.size());

        openZone(what.mTimer, commands, "lines");

        // Drawn over what the curve wrote, and left where the curve left it: the interface and
        // the presenter both take the target from there.
        Image& target = what.mTarget;
        target.transition(commands, Use::sComputeWrite, Use::sColourAttachment);

        mLines.record(commands,
            Lines{
                .mTarget = target,
                .mDepth = channels.get(Channel::Depth),
                .mConstants = {
                    .mCamera = Shaders::cameraOnGrid(what.mSampled.mCamera, target.getWidth(), target.getHeight()),
                    .mOrigin = what.mSampled.mOrigin,
                    .mNear = what.mSampled.mNear,
                    .mTraced = Shaders::uvec2(channels.getWidth(), channels.getHeight()),
                },
                .mVertices = what.mDebugVertices->getHandle(),
                .mLineCount = static_cast<std::uint32_t>(debug.mLines.size()),
                .mTriangleCount = static_cast<std::uint32_t>(debug.mTriangles.size()),
            });

        target.transition(commands, Use::sColourAttachment, Use::sComputeWrite);

        closeZone(what.mTimer, commands);
    }
}
