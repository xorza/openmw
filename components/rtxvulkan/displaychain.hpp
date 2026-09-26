#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <variant>

#include <vulkan/vulkan_core.h>

#include <components/rtx/debuglines.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/sunglare.hpp>

#include "bloompass.hpp"
#include "exposurepass.hpp"
#include "frameslots.hpp"
#include "linepass.hpp"
#include "sunglarepass.hpp"
#include "tonepass.hpp"

namespace Rtx
{
    class Buffer;
    class Device;
    class GBuffer;
    class GpuTimer;
    class Image;
    class VisibilityPass;
    struct TraceResult;

    /// What one picture asks of the display chain, and what makes a frame's different from a
    /// picture's inside the interface: a picture has no lens, no share of the sun, no eye of its
    /// own and no lines over it. Nothing here is held.
    struct Display
    {
        /// What the trace bound and the sprite tile list it read. The frame as it will be shown is
        /// the inputs' `mShown`, in `Use::sTraceReadWrite`: the puffs go over it and the curve
        /// maps it. The upscaler's output where one runs, the trace's own composite where none
        /// does, and a picture's own colour inside the interface.
        const TraceResult& mTrace;

        /// How much of the shown frame the picture is, from its corner: the whole of a frame's,
        /// and a picture's own size inside an image that may be larger. The curve encodes as much
        /// of `mTarget` from its corner.
        VkExtent2D mExtent;

        /// The camera the trace sampled, which the curve and the lines are told.
        const Shaders::VisibilityConstants& mSampled;

        /// What the curve writes into, at least `mExtent` large.
        Image& mTarget;

        /// The eye adapts off the shown frame at its own rate — from nothing where `mReset` says the
        /// camera has no past — or is held at a value, or a picture is measured off nothing —
        /// `ExposurePass::getPictureExposure` says why that is a buffer of its own.
        struct Measured
        {
            float mSeconds;
            bool mReset;
            float mBias;
        };
        struct Fixed
        {
            float mValue;
        };
        struct Picture
        {
        };
        using Exposure = std::variant<Measured, Fixed, Picture>;
        Exposure mExposure;

        /// Whether the lens spreads the picture: a picture inside the interface is a diagram.
        bool mBloom = false;

        /// The sun glare fader and the sun's share, eased at the query's own rate, or nothing for a
        /// picture, which is mapped with no glare at all.
        struct Glare
        {
            SunGlare mFader;
            float mSeconds;
            bool mReset;
        };
        std::optional<Glare> mGlare;

        /// The debug modes' lines and triangles over the picture, and the slot's own buffer they
        /// are drawn from: the frame behind read its own slot's, so nothing here is written under
        /// a submit. Nothing, and no buffer, for a picture.
        DebugLines mDebug;
        Buffer* mDebugVertices = nullptr;

        /// Null where the run is not being timed, which a picture is not.
        GpuTimer* mTimer = nullptr;
    };

    /// What comes after the trace and the upscaler: the puffs over the picture, the lens, the eye,
    /// the sun's share, the curve and the lines over the result. One chain for the frame and for
    /// every picture inside the interface, which differ in what they ask of it and in nothing
    /// else, so what happens between a finished trace and a target cannot be told two ways.
    class DisplayChain
    {
    public:
        /// @param puffs the trace's own pass, which composites the sprites over what was traced.
        /// @param textureLayout the scene's bindless textures, which the curve samples the star
        ///        sheet out of.
        /// @param targetFormat what the curve writes and the lines draw over.
        DisplayChain(const Device& device, const VisibilityPass& puffs, VkDescriptorSetLayout textureLayout,
            const std::filesystem::path& shaders, VkFormat targetFormat);

        /// The lens over `width` by `height`, which is what the frame is by the time the curve
        /// maps it: the upscaler's output where one runs and the trace's own extent where none
        /// does. A pyramid built at the other extent is a bloom at the wrong scale.
        void resize(std::uint32_t width, std::uint32_t height);

        /// The glare fader's query starts the frame at nothing, ahead of the trace that counts.
        void beginGlare(VkCommandBuffer commands) const;

        /// The two counts the eye's launch adds to, `SunGlarePass::getCounts`.
        const Buffer& getGlareCounts() const { return mSunGlare.getCounts(); }

        /// Says the measured exposure and the glare's eased share are worthless, each until the next
        /// frame that eases it: the share eases on every frame, and the exposure only on a frame
        /// that measures.
        void resetHistory() { mExposureStale = mGlareStale = true; }

        /// Records everything from the puffs to the target, and leaves `what.mTarget` in
        /// `Use::sComputeWrite`, where the curve left it.
        void record(VkCommandBuffer commands, const Display& what);

    private:
        /// Draws `what.mDebug` over the target after the curve, from the slot's own vertex
        /// buffer, depth-tested against the channels. Nothing at all for a picture with none,
        /// which is nearly every frame.
        void recordDebugLines(VkCommandBuffer commands, const Display& what);

        const Device& mDevice;
        const VisibilityPass& mPuffs;

        BloomPass mBloom;
        ExposurePass mExposure;

        /// How much of the sun's quad the frame's rays could see, eased, which the frame's curve
        /// lays the glare fader over the picture by.
        SunGlarePass mSunGlare;
        TonePass mTone;

        /// The debug modes' lines and triangles, over the picture and under the interface.
        LinePass mLines;

        /// Set by `resetHistory` and each spent by the next record that eases its history.
        bool mExposureStale = false;
        bool mGlareStale = false;
    };
}
