#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/png.hpp>

#include <apps/rtxtool/verify.hpp>

namespace RtxTool
{
    namespace
    {
        /// A picture of one flat colour, so a test can then move exactly the pixels it means to.
        Rtx::PngImage flat(std::uint32_t width, std::uint32_t height, std::uint8_t value)
        {
            Rtx::PngImage image{ width, height, {} };
            image.mPixels.assign(std::size_t{ width } * height * 4, value);
            return image;
        }

        std::uint8_t& channelAt(Rtx::PngImage& image, std::uint32_t x, std::uint32_t y, std::size_t channel)
        {
            return image.mPixels[(std::size_t{ y } * image.mWidth + x) * 4 + channel];
        }

        /// The whole of what the report says, on a picture whose differences are counted by hand.
        ///
        /// Ten by ten is a hundred pixels, so one pixel is exactly one percent and the fraction has
        /// nothing rounded in it.
        TEST(RtxVerifyTest, aDifferenceIsCountedInPixelsAndMeasuredInChannels)
        {
            const Rtx::PngImage before = flat(10, 10, 100);
            Rtx::PngImage after = before;

            EXPECT_TRUE(compareFrames(before, after).same());
            EXPECT_EQ(compareFrames(before, after).mTotal, 100u);

            // One pixel, off by two in green; and a second off by thirty-seven in red, which is what
            // the worst has to come back as.
            channelAt(after, 3, 4, 1) = 102;
            channelAt(after, 7, 1, 0) = 63;

            const FrameDifference difference = compareFrames(before, after);
            EXPECT_FALSE(difference.same());
            EXPECT_FALSE(difference.mMismatched);
            EXPECT_EQ(difference.mDiffering, 2u);
            EXPECT_EQ(difference.mTotal, 100u);
            EXPECT_EQ(difference.mWorst, 37u);
            EXPECT_DOUBLE_EQ(difference.getPercent(), 2.0);
        }

        /// **Colour only, and the reason is that alpha is not the picture.** The tone curve writes a
        /// constant there; a change confined to it is one nobody can see, and counting it would put
        /// a magnitude on a frame that is identical.
        TEST(RtxVerifyTest, alphaIsNotPartOfWhatThePictureLooksLike)
        {
            const Rtx::PngImage before = flat(4, 4, 200);
            Rtx::PngImage after = before;
            for (std::uint32_t y = 0; y < 4; ++y)
                for (std::uint32_t x = 0; x < 4; ++x)
                    channelAt(after, x, y, 3) = 0;

            EXPECT_TRUE(compareFrames(before, after).same());
        }

        /// Two sizes are not a delta, and neither is a reference that was never written.
        TEST(RtxVerifyTest, nothingToSubtractIsSaidRatherThanCountedAsZero)
        {
            const Rtx::PngImage before = flat(10, 10, 100);

            EXPECT_TRUE(compareFrames(before, flat(10, 9, 100)).mMismatched);
            EXPECT_TRUE(compareFrames(before, Rtx::PngImage{}).mMismatched);
            EXPECT_TRUE(compareFrames(Rtx::PngImage{}, before).mMismatched);

            // And a mismatch is never `same`, which is what the exit status is built on.
            EXPECT_FALSE(compareFrames(before, Rtx::PngImage{}).same());
        }
    }
}
