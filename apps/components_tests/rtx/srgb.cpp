#include <cstdint>

#include <gtest/gtest.h>

#include <components/rtx/srgb.hpp>

namespace Rtx
{
    namespace
    {
        /// The byte overload is the float curve at that byte, and not an approximation of it.
        ///
        /// **Every one of the two hundred and fifty-six, bit for bit.** A texel arrives as a byte, so
        /// the only arguments the curve is ever given by a decoder are `k / 255` — which is what
        /// makes a table over `k` the same answer rather than a faster one. A `NEAR` here would say
        /// the pictures may differ by a rounding, and they may not.
        TEST(RtxSrgbTest, theByteCurveIsTheFloatCurveAtEveryStoredValue)
        {
            for (std::uint32_t stored = 0; stored < 256; ++stored)
            {
                const auto byte = static_cast<std::uint8_t>(stored);
                EXPECT_EQ(toLinear(byte), toLinear(static_cast<float>(stored) / 255.0f)) << "at " << stored;
            }
        }

        /// The curve itself, anchored where it can be stated outright.
        ///
        /// **What the test above cannot say.** That one compares two spellings of the curve, so it
        /// passes whether or not either is right. These are the three points sRGB names.
        TEST(RtxSrgbTest, nothingStaysNothingAndFullStaysFull)
        {
            EXPECT_EQ(toLinear(std::uint8_t{ 0 }), 0.0f);
            EXPECT_EQ(toLinear(std::uint8_t{ 255 }), 1.0f);

            // The knee, which is the one place the curve is two rules: 10 / 255 = 0.0392 is under
            // `0.04045` and takes the linear leg, and 11 / 255 = 0.0431 is over it and takes the
            // power leg, which lands above what the linear one would have given.
            EXPECT_EQ(toLinear(std::uint8_t{ 10 }), 10.0f / 255.0f / 12.92f);
            EXPECT_GT(toLinear(std::uint8_t{ 11 }), 11.0f / 255.0f / 12.92f);
        }
    }
}
