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
    /// the frames before it.
    ///
    /// **The wavelet behind it was the second half of a denoiser with no first half.** Five spatial
    /// levels blurring a single sample per pixel is where the field started and not where it
    /// settled: SVGF, A-SVGF, ReLAX and ReBLUR are all a temporal accumulator with a cascade
    /// attached, and the cascade is there to fill in where the accumulator was rejected rather than
    /// to do the averaging itself. An average over frames is where the estimator's error actually
    /// falls, and the variance it produces on the way is what lets the cascade finally stop at an
    /// edge in the light.
    ///
    /// **It runs exactly when the wavelet does.** Ray Reconstruction accumulates over frames itself,
    /// so a frame it is handed must not have been accumulated already — `Reconstruction::resolve`
    /// answers with one denoiser or the other and never both, and this belongs to the one.
    class AccumulatePass
    {
    public:
        AccumulatePass(const Device& device, const std::filesystem::path& shaderDirectory);

        AccumulatePass(const AccumulatePass&) = delete;
        AccumulatePass& operator=(const AccumulatePass&) = delete;

        /// Makes room for a frame this size, if the last one was not. Before the first frame.
        ///
        /// **A resize is a reset**, because a history at the old size describes pixels that are no
        /// longer where it says. The caller is expected to have waited for anything still reading
        /// the old images.
        void resize(std::uint32_t width, std::uint32_t height);

        /// Blends the buffer's indirect channel with the history, and leaves this frame's moments
        /// and its blend where the cascade can read them.
        ///
        /// **Into an image of this pass's own, and it used to be back over the channel it read.**
        /// The cascade overwrites its own input as it ping-pongs, so the trace's answer survived
        /// only as long as nothing filtered the frame — which made `Channel::Indirect` two different
        /// things and tied the two passes to one format. `getBlended` is where the blend is now.
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

        /// Where the cascade's first level writes the mean this pass will read next frame.
        ///
        /// **Handed out rather than written here.** SVGF feeds the first wavelet level's output back
        /// as the history, so what carries forward is the filtered light and not the one sample this
        /// pass blended into it — and the write lands on the cascade, which is bound by the work per
        /// tap, instead of on this pass, which is bound by its bytes. Only valid after `record`,
        /// which is what picks the half of the pair the next frame will read.
        const Image& getHistory() const;

    private:
        const Device& mDevice;
        ComputePipeline mPipeline;

        /// **Two of each, because this frame reads what the last one wrote and writes what the next
        /// one will read.** A single set would be a pixel averaging with itself.
        ///
        /// The mean, written by the cascade rather than here; the surface that mean belongs to, so
        /// a reprojection can ask whether it is still looking at the same thing; and the two moments
        /// of its luminance, which is where the variance comes from and where the frame count sits.
        /// Null until `resize`.
        std::array<std::unique_ptr<Image>, 2> mColour;
        std::array<std::unique_ptr<Image>, 2> mSurface;
        std::array<std::unique_ptr<Image>, 2> mMoments;

        /// Where the blend goes, in the cascade's format because the cascade both reads and
        /// overwrites it. Readable, so `Channel::Accumulated` can hand it back. Null until `resize`.
        std::unique_ptr<Image> mBlended;

        /// Which half of each pair this frame writes. Flipped by `record`.
        std::size_t mCurrent = 0;

        /// Set by `resize`, so the first frame after one does not read an image nothing has written.
        bool mFresh = true;
    };
}
