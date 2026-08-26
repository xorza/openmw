#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Image>

#include <components/rtx/frameimage.hpp>

namespace Rtx
{
    namespace
    {
        /// One texel, in the order a backend hands them over.
        constexpr std::array<std::uint8_t, 4> texel(std::uint8_t red, std::uint8_t green)
        {
            return { red, green, 0, 255 };
        }

        /// A frame `width` by `height` whose every texel names its own column in red and its own row
        /// in green, so what a resample picked and which way up it is are both readable.
        std::vector<std::uint8_t> makeFrame(int width, int height)
        {
            std::vector<std::uint8_t> pixels;
            pixels.reserve(static_cast<std::size_t>(width) * height * 4);

            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    for (const std::uint8_t channel : texel(static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)))
                        pixels.push_back(channel);

            return pixels;
        }

        /// The red and green of one texel of a result.
        std::pair<std::uint8_t, std::uint8_t> at(const osg::Image& image, int x, int y)
        {
            const std::uint8_t* row = image.data(0, y);
            return { row[x * 4], row[x * 4 + 1] };
        }

        /// The whole frame comes back at the frame's own extents, and the row order is the only
        /// thing that separates the two callers.
        ///
        /// **Which is the whole of what a load screen and a screenshot need to be right.** The
        /// loading screen puts this up as its backdrop through `MyGUIPlatform::Picture`, which copies
        /// an image straight into a locked texture and draws it downwards; the screenshot goes to
        /// `osgDB`, which reads an image upwards. One of the two is upside down if this flips for
        /// both or for neither.
        TEST(RtxFrameImageTest, aFrameComesBackAtItsOwnExtentsAndOnlyTheRowOrderDiffers)
        {
            const std::vector<std::uint8_t> pixels = makeFrame(2, 2);
            const TracedFrame frame{ .mWidth = 2, .mHeight = 2, .mPixels = pixels };

            const osg::ref_ptr<osg::Image> top = frameImage(frame, 2, 2, RowOrder::TopFirst);
            ASSERT_NE(top, nullptr);
            EXPECT_EQ(top->s(), 2);
            EXPECT_EQ(top->t(), 2);
            EXPECT_EQ(top->getPixelFormat(), GL_RGBA);

            // The trace's own order, kept: row zero of the result is row zero of the frame.
            EXPECT_EQ(at(*top, 0, 0), (std::pair<std::uint8_t, std::uint8_t>{ 0, 0 }));
            EXPECT_EQ(at(*top, 1, 0), (std::pair<std::uint8_t, std::uint8_t>{ 1, 0 }));
            EXPECT_EQ(at(*top, 0, 1), (std::pair<std::uint8_t, std::uint8_t>{ 0, 1 }));
            EXPECT_EQ(at(*top, 1, 1), (std::pair<std::uint8_t, std::uint8_t>{ 1, 1 }));

            const osg::ref_ptr<osg::Image> bottom = frameImage(frame, 2, 2, RowOrder::BottomFirst);
            ASSERT_NE(bottom, nullptr);
            EXPECT_EQ(bottom->s(), 2);
            EXPECT_EQ(bottom->t(), 2);

            // Turned over: row zero of the result is the last row of the frame, and the columns are
            // untouched — a flip of both axes would pass a test that only looked at row zero.
            EXPECT_EQ(at(*bottom, 0, 0), (std::pair<std::uint8_t, std::uint8_t>{ 0, 1 }));
            EXPECT_EQ(at(*bottom, 1, 0), (std::pair<std::uint8_t, std::uint8_t>{ 1, 1 }));
            EXPECT_EQ(at(*bottom, 0, 1), (std::pair<std::uint8_t, std::uint8_t>{ 0, 0 }));
            EXPECT_EQ(at(*bottom, 1, 1), (std::pair<std::uint8_t, std::uint8_t>{ 1, 0 }));
        }

        /// A thumbnail is nearest-sampled, and which texels it lands on is arithmetic rather than
        /// taste.
        ///
        /// Hand-computed for four across to two: a target column takes source `x * 4 / 2`, so
        /// columns 0 and 2; a target row takes the same, so rows 0 and 2 read downwards. Read
        /// upwards the target rows are 1 and 0 of the target, which land on source rows 2 and 0.
        TEST(RtxFrameImageTest, aThumbnailTakesTheNearestTexelAndTheSizeItWasAskedFor)
        {
            const std::vector<std::uint8_t> pixels = makeFrame(4, 4);
            const TracedFrame frame{ .mWidth = 4, .mHeight = 4, .mPixels = pixels };

            const osg::ref_ptr<osg::Image> top = frameImage(frame, 2, 2, RowOrder::TopFirst);
            ASSERT_NE(top, nullptr);
            EXPECT_EQ(top->s(), 2);
            EXPECT_EQ(top->t(), 2);

            EXPECT_EQ(at(*top, 0, 0), (std::pair<std::uint8_t, std::uint8_t>{ 0, 0 }));
            EXPECT_EQ(at(*top, 1, 0), (std::pair<std::uint8_t, std::uint8_t>{ 2, 0 }));
            EXPECT_EQ(at(*top, 0, 1), (std::pair<std::uint8_t, std::uint8_t>{ 0, 2 }));
            EXPECT_EQ(at(*top, 1, 1), (std::pair<std::uint8_t, std::uint8_t>{ 2, 2 }));

            const osg::ref_ptr<osg::Image> bottom = frameImage(frame, 2, 2, RowOrder::BottomFirst);
            ASSERT_NE(bottom, nullptr);
            EXPECT_EQ(at(*bottom, 0, 0), (std::pair<std::uint8_t, std::uint8_t>{ 0, 2 }));
            EXPECT_EQ(at(*bottom, 1, 1), (std::pair<std::uint8_t, std::uint8_t>{ 2, 0 }));
        }

        /// Nothing to give is null, not a picture of part of a frame.
        ///
        /// **The first load is the case that matters.** `freezeFrame` is asked before anything has
        /// been presented, so the extents are zero and there is no readback behind them; a screenshot
        /// key pressed at the same moment is the same question. Answering with whatever the buffer
        /// happened to hold is how a loading screen comes up showing the last session.
        TEST(RtxFrameImageTest, aFrameThatIsNotThereIsRefusedRatherThanPartlyDrawn)
        {
            const std::vector<std::uint8_t> pixels = makeFrame(2, 2);

            EXPECT_EQ(frameImage(TracedFrame{}, 2, 2, RowOrder::TopFirst), nullptr) << "no frame at all";

            EXPECT_EQ(frameImage(TracedFrame{ .mWidth = 2, .mHeight = 2, .mPixels = std::span(pixels).first(4) }, 2, 2,
                          RowOrder::TopFirst),
                nullptr)
                << "one texel standing in for four";

            const TracedFrame whole{ .mWidth = 2, .mHeight = 2, .mPixels = pixels };
            EXPECT_EQ(frameImage(whole, 0, 2, RowOrder::TopFirst), nullptr) << "asked for no width";
            EXPECT_EQ(frameImage(whole, 2, -1, RowOrder::TopFirst), nullptr) << "asked for a negative height";

            // And the frame it does have comes back, so the refusals above are not simply everything.
            EXPECT_NE(frameImage(whole, 2, 2, RowOrder::TopFirst), nullptr);
        }
    }
}
