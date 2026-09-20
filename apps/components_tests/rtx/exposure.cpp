#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include <components/rtx/shaders/exposure.h>

namespace Rtx
{
    namespace
    {
        /// The two halves of the bin mapping are inverses of each other, and a luminance lands in
        /// the bin that stands for it.
        ///
        /// **Hand-placed at the scale's ends and its middle.** The scale runs from `2^-10` at bin
        /// one to `2^6` at bin `EXPOSURE_BINS - 1`, so a luminance of one is `10 / 16` of the way
        /// along it: `0.625 * 254` is 158.75, and the bin under that plus the black bin is 159.
        TEST(RtxExposureBinTest, aLuminanceLandsInTheBinThatStandsForIt)
        {
            EXPECT_EQ(Shaders::luminanceBin(std::exp2(Shaders::MIN_LOG_LUMINANCE)), 1u) << "the bottom of the scale";
            EXPECT_EQ(Shaders::luminanceBin(std::exp2(Shaders::MAX_LOG_LUMINANCE)), Shaders::EXPOSURE_BINS - 1u)
                << "the top of it";
            EXPECT_EQ(Shaders::luminanceBin(1.0f), 159u) << "and mid grey";

            // **The middle of each bin and not its edge.** `binLuminance` of a whole bin is the
            // luminance at that bin's lower edge, and `log2` of `exp2` of it lands a rounding either
            // side of the edge; half a bin in, the two round trips agree by a margin of half a bin.
            for (std::uint32_t bin = 1; bin < Shaders::EXPOSURE_BINS; ++bin)
            {
                const float middle = Shaders::binLuminance(static_cast<float>(bin) + 0.5f);
                EXPECT_EQ(Shaders::luminanceBin(middle), bin) << "a bin's middle lands in it, at " << bin;
            }
        }

        /// Bin nought is black and off the scale: below `EXPOSURE_BLACK` nothing is binned, and
        /// a luminance past either end is held to the end rather than to a bin that does not exist.
        TEST(RtxExposureBinTest, blackIsTheBinBelowTheScaleAndTheEndsHold)
        {
            EXPECT_EQ(Shaders::luminanceBin(0.0f), 0u);
            EXPECT_EQ(Shaders::luminanceBin(std::nextafter(Shaders::EXPOSURE_BLACK, 0.0f)), 0u);

            EXPECT_EQ(Shaders::luminanceBin(std::exp2(Shaders::MIN_LOG_LUMINANCE) * 0.5f), 1u) << "under the scale";
            EXPECT_EQ(Shaders::luminanceBin(1.0e6f), Shaders::EXPOSURE_BINS - 1u) << "over it";
        }
    }
}
