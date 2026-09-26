#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "image.hpp"

namespace Rtx
{
    class Device;

    /// One camera's history for `AccumulatePass`, at one extent: what the last frame left for this
    /// one, and where this one's blend goes. A chain's and not the pass's, because the pass is a
    /// pipeline every chain shares and a history is as big as the camera it follows.
    class AccumulateHistory
    {
    public:
        explicit AccumulateHistory(const Device& device);

        /// Makes room for a frame this size, if the last one was not. A resize is a reset. The
        /// caller has waited for anything still reading the old images.
        void resize(std::uint32_t width, std::uint32_t height);

        /// Says the history is worthless, until the next `turn`: the frame after it starts again
        /// as the first after a resize does.
        void reset() { mFresh = true; }

        /// What one frame reads and writes: the half of each pair the last frame wrote, the half
        /// this one writes, the blend, and whether there is any history at all.
        struct Turn
        {
            const Image& mColourBefore;
            const Image& mSurfaceBefore;
            const Image& mMomentsBefore;
            const Image& mColour;
            const Image& mSurface;
            const Image& mMoments;
            const Image& mBlended;

            /// The first frame after a `resize` or a `reset`, whose history is worthless.
            bool mFresh = false;
        };

        /// Turns to the other half of each pair for the frame being recorded. From here on the
        /// history is the one this frame hands over.
        Turn turn();

        /// This frame's bounce blended with the history, which is what the cascade filters. One
        /// image and not a pair, because nothing reads it after the frame that wrote it: the
        /// cascade consumes it immediately and the history the next frame needs is the colour.
        const Image& getBlended() const;

        /// Where the cascade's first level writes the mean the pass reads next frame — SVGF's
        /// feedback, so what carries forward is the filtered light. Only valid after `turn`.
        const Image& getHistory() const;

        std::uint32_t getWidth() const { return mBlended.getWidth(); }
        std::uint32_t getHeight() const { return mBlended.getHeight(); }

    private:
        const Device& mDevice;

        /// Two of each, because this frame reads what the last one wrote: the mean, written by the
        /// cascade; the surface it belongs to, for the reprojection; and the two moments of its
        /// luminance, where the variance and the frame count sit. Empty until `resize`.
        std::array<Image, 2> mColour;
        std::array<Image, 2> mSurface;
        std::array<Image, 2> mMoments;

        /// Where the blend goes, in the cascade's format because the cascade both reads and
        /// overwrites it. Empty until `resize`.
        Image mBlended;

        /// Which half of each pair this frame writes. Flipped by `turn`.
        std::size_t mCurrent = 0;

        /// Set by `resize`, so the first frame after one does not read an image nothing has
        /// written, and by `reset`.
        bool mFresh = true;
    };
}
