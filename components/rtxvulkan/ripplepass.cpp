#include "ripplepass.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include <osg/Vec2f>
#include <osg/Vec2i>

#include <components/rtx/wavecascade.hpp>

#include "barriers.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "dispatch.hpp"
#include "gputimer.hpp"
#include "imageuse.hpp"
#include "pipeline.hpp"

namespace Rtx
{
    namespace
    {
        /// The field a step reads, the one it writes, and the impulses it presses.
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> sStepBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        };

        /// The field in, and the two tiles out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> sComposeBindings
            = computeBindings<3>(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

        constexpr VkFormat sFieldFormat = VK_FORMAT_R32G32_SFLOAT;

        constexpr std::uint32_t sGrid = Shaders::RIPPLE_GRID;

        /// Which sixtieth of the sky's clock `seconds` falls in.
        ///
        /// **A count and not a remainder, so the arithmetic cannot drift.** A remainder carried
        /// in floating point fell a few ulps short of a sixtieth every so often and skipped the
        /// step; a tick is a floor, and a thousandth of a tick of slack keeps a clock that lands
        /// exactly on a sixtieth on the right side of it.
        std::int64_t tickOf(const double seconds)
        {
            return static_cast<std::int64_t>(
                std::floor(seconds * static_cast<double>(Shaders::RIPPLE_STEP_RATE) + 1.0e-3));
        }

        std::uint32_t groups()
        {
            return groupsFor(sGrid, Shaders::RIPPLE_WORKGROUP);
        }
    }

    RipplePass::RipplePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mDevice(device)
        , mStepPipeline(device, sStepBindings, sizeof(Shaders::RippleStepConstants), {},
              shaderDirectory / "ripplestep.comp.spv", "ripple step")
        , mComposePipeline(
              device, sComposeBindings, 0, {}, shaderDirectory / "ripplecompose.comp.spv", "ripple compose")
        , mSampler(makeBorderSampler(device, "ripples"))
        , mImpulses([&](const FrameSlot) {
            return Buffer::hostWritten(device, Shaders::RIPPLE_IMPULSES_MOST * sizeof(Shaders::GpuRippleImpulse),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "ripple impulses");
        })
    {
        constexpr VkImageUsageFlags fieldUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        constexpr VkImageUsageFlags tileUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        const std::uint32_t levels = levelsFor(sGrid);

        mFields[0] = Image(device, sGrid, sGrid, sFieldFormat, fieldUsage, "ripple field 0");
        mFields[1] = Image(device, sGrid, sGrid, sFieldFormat, fieldUsage, "ripple field 1");
        mSurface = Image(device, sGrid, sGrid, WAVE_TILE_FORMAT, tileUsage, "ripple surface", levels);
        mCurvature = Image(device, sGrid, sGrid, WAVE_TILE_FORMAT, tileUsage, "ripple curvature", levels);

        mImpulseScratch.reserve(Shaders::RIPPLE_IMPULSES_MOST);
        mPending.reserve(Shaders::RIPPLE_IMPULSES_MOST);

        // Still water in every tile from the first frame, in the layout the trace samples them in:
        // a frame with no water records nothing here and binds them anyway.
        mDevice.getPool().submitAndWait([&](VkCommandBuffer commands) {
            const VkClearColorValue still{ .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
            for (const Image* image : { &mSurface, &mCurvature })
                image->clear(commands, Use::sUndefined, still, Use::sShaderSample);
            for (const Image& field : mFields)
                field.clear(commands, Use::sUndefined, still, Use::sComputeReadWrite);
        });
    }

    osg::Vec2i RipplePass::windowOf(const osg::Vec2f& eye)
    {
        const auto texel = [](const float along) {
            return static_cast<int>(std::floor(along / Shaders::RIPPLE_TEXEL)) - static_cast<int>(sGrid / 2);
        };

        return osg::Vec2i(texel(eye.x()), texel(eye.y()));
    }

    void RipplePass::standWindow(const osg::Vec2i& window)
    {
        mWindow = window;
        mOrigin = osg::Vec2f(static_cast<float>(window.x()), static_cast<float>(window.y())) * Shaders::RIPPLE_TEXEL;
    }

    void RipplePass::record(const VkCommandBuffer commands, const FrameSlot slot,
        const std::span<const RippleImpulse> impulses, const osg::Vec2f& eye, const double skySeconds,
        GpuTimer* const timer)
    {
        // A field that was reset, or never started, stands still where the eye is now and owes
        // nothing to where it stood.
        if (mReset)
        {
            standWindow(windowOf(eye));
            mSteppedTick = tickOf(skySeconds);
            mReset = false;

            const VkClearColorValue still{ .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
            for (const Image& field : mFields)
                field.clear(commands, Use::sComputeReadWrite, still, Use::sComputeReadWrite);
            for (const Image* image : { &mSurface, &mCurvature })
                image->clear(commands, Use::sShaderSample, still, Use::sShaderSample);
        }

        // What this frame disturbed, kept until the step that presses it: a frame that comes round
        // before a sixtieth has accrued steps nothing, and its footfalls wait for the step rather
        // than being dropped. Capped at what the buffer holds, oldest first.
        for (const RippleImpulse& impulse : impulses)
            if (mPending.size() < Shaders::RIPPLE_IMPULSES_MOST)
                mPending.push_back(impulse);

        // One step where a sixtieth has passed, as `RipplesSurface::updateState` steps: a frame
        // that comes round before one is due neither steps nor moves the window, and a frame that
        // took several steps a second still steps once. A clock that ran backwards stands still.
        const std::int64_t tick = tickOf(skySeconds);
        if (tick <= mSteppedTick)
            return;

        mSteppedTick = tick;

        openZone(timer, commands, "ripples");

        // The window follows the eye by whole texels, and the step reads the old field at the
        // offset the window moved by.
        const osg::Vec2i window = windowOf(eye);
        const osg::Vec2i shift = window - mWindow;
        standWindow(window);

        // The impulses in the new window's texels.
        mImpulseScratch.clear();
        for (const RippleImpulse& impulse : mPending)
            mImpulseScratch.push_back(Shaders::GpuRippleImpulse{
                .mAt = (impulse.mAt - mOrigin) / Shaders::RIPPLE_TEXEL,
                .mRadius = impulse.mSize / Shaders::RIPPLE_TEXEL,
                .mStrength = -1.0f,
            });
        mPending.clear();

        Buffer& impulseBuffer = mImpulses.at(slot);
        if (!mImpulseScratch.empty())
            impulseBuffer.write(std::span<const Shaders::GpuRippleImpulse>(mImpulseScratch));

        const Image& before = mFields[mLatest];
        const Image& after = mFields[1 - mLatest];
        mLatest = 1 - mLatest;

        DescriptorWrites<3> steps;
        steps.image(0, before.describeStorage());
        steps.image(1, after.describeStorage());
        steps.buffer(2, impulseBuffer.describe());

        const Shaders::RippleStepConstants stepped{
            .mShift = shift,
            .mCount = static_cast<std::uint32_t>(mImpulseScratch.size()),
        };

        dispatch(commands, mStepPipeline, steps.get(), stepped, groups(), groups());

        // The step wrote what the compose reads, and what the next step reads back.
        handOver(commands, Use::sBufferComputeWrite, Use::sBufferComputeReadWrite);

        Barriers opened(commands);
        for (const Image* image : { &mSurface, &mCurvature })
            opened.add(image->describeTransition(Use::sShaderSample, Use::sComputeWrite));
        opened.flush();

        DescriptorWrites<3> composes;
        composes.image(0, after.describeStorage());
        composes.image(1, mSurface.describeStorage());
        composes.image(2, mCurvature.describeStorage());

        // Bound and pushed by hand: the compose is told nothing, and `dispatch` pushes a block.
        bind(commands, mComposePipeline);
        pushDescriptors(commands, mComposePipeline, composes.get());
        vkCmdDispatch(commands, groups(), groups(), 1);

        for (const Image* image : { &mSurface, &mCurvature })
            image->buildMips(commands);

        closeZone(timer, commands);
    }
}
