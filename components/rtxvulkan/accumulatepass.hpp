#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/accumulate.h>
#include <components/rtx/shaders/atrous.h>

#include "computepipeline.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;
    class GBuffer;

    /// The denoiser's temporal half: this frame's bounce averaged with what the same surface gave on
    /// the frames before it. SVGF, A-SVGF, ReLAX and ReBLUR are all a temporal accumulator with a
    /// cascade attached, and the cascade fills in where the accumulator was rejected rather than
    /// doing the averaging itself. It runs exactly when the wavelet does, because Ray
    /// Reconstruction accumulates over frames itself.
    class AccumulatePass
    {
    public:
        AccumulatePass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Makes room for a frame this size, if the last one was not. A resize is a reset. The
        /// caller has waited for anything still reading the old images.
        void resize(std::uint32_t width, std::uint32_t height);

        /// Blends the buffer's indirect channel with the history, and leaves this frame's moments
        /// and its blend (`getBlended`) where the cascade can read them — into an image of this
        /// pass's own, or `Channel::Indirect` would mean two different things.
        ///
        /// @param far the frame's far plane, which this turns into a storage scale rather than
        ///        writing a depth against. `AccumulateConstants::mDistanceScale` says why it is a
        ///        parameter of its own instead of a field of `Camera`.
        /// @param reset true where there is no history worth carrying — the first frame, a resize, a
        ///        door walked through. The same signal Ray Reconstruction is handed.
        /// @return the moments image the cascade weighs its taps by.
        const Image& record(
            VkCommandBuffer commands, const GBuffer& buffer, const Shaders::Camera& camera, float far, bool reset);

        /// This frame's bounce blended with the history, which is what the cascade filters.
        ///
        /// **One image and not a pair**, because nothing reads it after the frame that wrote it: the
        /// cascade consumes it immediately and the history the next frame needs is `mColour`.
        const Image& getBlended() const;

        /// Where the cascade's first level writes the mean this pass will read next frame — SVGF's
        /// feedback, so what carries forward is the filtered light. Only valid after `record`.
        const Image& getHistory() const;

    private:
        const Device& mDevice;
        ComputePipeline mPipeline;

        /// Two of each, because this frame reads what the last one wrote: the mean, written by the
        /// cascade; the surface it belongs to, for the reprojection; and the two moments of its
        /// luminance, where the variance and the frame count sit. Null until `resize`.
        std::array<std::unique_ptr<Image>, 2> mColour;
        std::array<std::unique_ptr<Image>, 2> mSurface;
        std::array<std::unique_ptr<Image>, 2> mMoments;

        /// Where the blend goes, in the cascade's format because the cascade both reads and
        /// overwrites it. Readable, so `FrameImage::Accumulated` can hand it back. Null until `resize`.
        std::unique_ptr<Image> mBlended;

        /// Which half of each pair this frame writes. Flipped by `record`.
        std::size_t mCurrent = 0;

        /// Set by `resize`, so the first frame after one does not read an image nothing has written.
        bool mFresh = true;
    };
}
