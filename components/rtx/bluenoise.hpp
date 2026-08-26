#pragma once

#include <span>
#include <vector>

#include "shaders/scene.h"

namespace Rtx
{
    /// A square tile of blue noise: `RANDOM_STREAMS` independent masks over the same pixels.
    ///
    /// **Blue noise does not make a sample better, it makes the error easier to remove.** One bounce
    /// per pixel has the same variance however the direction was drawn; what changes is where that
    /// variance sits in frequency. Drawn from a hash, the error is white — spread evenly across the
    /// spectrum, so a share of it lands in the low frequencies as blotches, and no spatial filter can
    /// tell those from lighting that is genuinely smooth. Drawn from a tile like this one, the error
    /// is pushed into the high frequencies, which is both what a filter takes out cleanly and what
    /// the eye forgives while there is no filter yet.
    ///
    /// **Generated rather than shipped.** The usual answer is a binary mask distributed with the
    /// renderer; void-and-cluster is a hundred lines and gives something a test can check instead of
    /// trust — that the ranks are a permutation, and that the spectrum really has no low frequencies
    /// in it, which the same assertion run against a hash does not.
    class BlueNoise
    {
    public:
        /// The one tile, built on first use.
        ///
        /// It is a pure function of the constants above it, so a second would be the same numbers
        /// again — and building it costs a tenth of a second, which is worth paying once and not per
        /// pipeline.
        static const BlueNoise& shared();

        BlueNoise(const BlueNoise&) = delete;
        BlueNoise& operator=(const BlueNoise&) = delete;

        /// Values in `[0, 1)`, one pixel's `RANDOM_STREAMS` channels at a time, row by row.
        ///
        /// Interleaved rather than planar, so that the channels one pixel reads sit in one cache
        /// line: a shader that draws a pair touches two of them and never the same channel twice.
        std::span<const float> getValues() const { return mValues; }

    private:
        BlueNoise();

        std::vector<float> mValues;
    };
}
