#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <osg/Vec2f>
#include <osg/Vec2i>

#include <components/rtx/ripple.hpp>
#include <components/rtx/shaders/ripple.h>

#include "buffer.hpp"
#include "computepipeline.hpp"
#include "frameslots.hpp"
#include "handles.hpp"
#include "image.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;
    class GpuTimer;

    /// What walked through the water, as a height field around the eye the sea reads beside its
    /// cascades: stepped by the wave equation, pressed where the game says something disturbed
    /// it, and unpacked into a slope tile and a curvature tile with a chain each, in the wave
    /// tiles' own layout. `shaders/ripple.h` says whose field this is.
    ///
    /// **State across frames, and deterministic.** The field is what the steps and the impulses
    /// made of it, and both are the run's own — the step by the sky's clock, the impulses by the
    /// simulation — so two runs of one build stand the same field on every frame.
    class RipplePass
    {
    public:
        /// Makes the field still, and leaves every tile in the layout the trace samples it in.
        /// Submits and waits.
        RipplePass(const Device& device, CommandPool& pool, const std::filesystem::path& shaderDirectory);

        /// Steps the field once where a sixtieth has accrued on `skySeconds` since the last step,
        /// presses `impulses`, and unpacks the tiles. The window follows `eye` by whole texels.
        /// Nothing at all where no step is due and nothing is pressed, which leaves the tiles as
        /// they were.
        ///
        /// @param slot the frame's, which names the copy of the impulse buffer this frame writes.
        /// @param timer where the step's zone goes, or nothing where nobody is counting. Opened
        ///        only on a frame that steps, so a frame that stands still reports no zone.
        void record(VkCommandBuffer commands, FrameSlot slot, std::span<const RippleImpulse> impulses,
            const osg::Vec2f& eye, double skySeconds, GpuTimer* timer);

        /// Drops what the field holds, for a world that was replaced rather than moved through.
        void reset() { mReset = true; }

        /// Linear, mipmapped and clamped to a border of nothing: past the field's edge is still
        /// water.
        VkSampler getSampler() const { return mSampler.get(); }

        const Image& getSurface() const { return mSurface; }
        const Image& getCurvature() const { return mCurvature; }

        /// Where the field's window begins, in world units, and how wide it is.
        const osg::Vec2f& getOrigin() const { return mOrigin; }
        static float getExtent() { return Shaders::RIPPLE_TEXEL * static_cast<float>(Shaders::RIPPLE_GRID); }

    private:
        /// Which texel the window would begin at for an eye at `eye`: the eye's own texel less
        /// half the grid, so the eye stands in the middle.
        static osg::Vec2i windowOf(const osg::Vec2f& eye);

        /// Moves the window to `window`, in texels and in world units together.
        void standWindow(const osg::Vec2i& window);

        const Device& mDevice;
        CommandPool& mPool;

        ComputePipeline mStepPipeline;
        ComputePipeline mComposePipeline;

        Sampler mSampler;

        /// The two heights, this step's and the one before, ping-ponged between two images so a
        /// step reads one whole and writes the other.
        std::array<Image, 2> mFields;
        std::size_t mLatest = 0;

        Image mSurface;
        Image mCurvature;

        /// The frame's impulses, one copy a frame slot so a write never lands under a submit, and
        /// the ones waiting for a step to be due.
        PerSlot<Buffer> mImpulses;
        std::vector<Shaders::GpuRippleImpulse> mImpulseScratch;
        std::vector<RippleImpulse> mPending;

        /// Where the window begins, in texels and in world units.
        osg::Vec2i mWindow;
        osg::Vec2f mOrigin;

        /// The sky's clock at the last step, in whole sixtieths.
        std::int64_t mSteppedTick = 0;
        bool mReset = true;
    };
}
