#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Image>

#include <components/myguiplatform/pixels.hpp>

namespace MyGUIPlatform
{
    namespace
    {
        /// A grey picture `width` by `height`, with `value(x, y)` in every channel of every pixel.
        osg::ref_ptr<osg::Image> makeGrey(int width, int height, const std::function<int(int, int)>& value)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    std::uint8_t* texel = image->data(x, y);
                    for (int channel = 0; channel < 4; ++channel)
                        texel[channel] = static_cast<std::uint8_t>(value(x, y));
                }
            }

            return image;
        }

        /// A tap between four texels weighs all four, and the weights are the distances.
        ///
        /// A two-by-two picture sampled dead centre sits half a texel from each of them, so the
        /// answer is their mean: (0 + 100 + 200 + 255) / 4 = 138.75, which lands on 139.
        TEST(MyGUIPlatformPixelsTest, aTapBetweenFourTexelsWeighsThemByDistance)
        {
            const osg::ref_ptr<osg::Image> image = makeGrey(2, 2, [](int x, int y) {
                constexpr int corners[2][2] = { { 0, 200 }, { 100, 255 } };
                return corners[x][y];
            });

            std::uint8_t sampled[4];
            sampleBilinear(*image, 0.5f, 0.5f, sampled);

            for (int channel = 0; channel < 4; ++channel)
                EXPECT_EQ(sampled[channel], 139) << "channel " << channel;
        }

        /// A tap on a texel's own centre is that texel and nothing of its neighbours.
        ///
        /// Centres sit at half-integers, so on a four-wide picture texel two is at
        /// (2 + 0.5) / 4 = 0.625.
        TEST(MyGUIPlatformPixelsTest, aTapOnATexelCentreIsThatTexel)
        {
            const osg::ref_ptr<osg::Image> image = makeGrey(4, 1, [](int x, int) { return x * 60; });

            std::uint8_t sampled[4];
            sampleBilinear(*image, 0.625f, 0.5f, sampled);

            EXPECT_EQ(sampled[0], 120);

            sampleBilinear(*image, 0.375f, 0.5f, sampled);
            EXPECT_EQ(sampled[0], 60);
        }

        /// Halfway between two centres is their mean, which is what makes the reduction a blend of
        /// two texels rather than a pick of one.
        ///
        /// Texel one is at 0.375 and texel two at 0.625, so 0.5 is the midpoint of 60 and 120.
        TEST(MyGUIPlatformPixelsTest, aTapBetweenTwoCentresIsTheirMean)
        {
            const osg::ref_ptr<osg::Image> image = makeGrey(4, 1, [](int x, int) { return x * 60; });

            std::uint8_t sampled[4];
            sampleBilinear(*image, 0.5f, 0.5f, sampled);

            EXPECT_EQ(sampled[0], 90);
        }

        /// Outside the outermost centres the edge texel is held, rather than the tap wrapping to the
        /// other side of the picture.
        ///
        /// **This is what the sampler the overlay used to go through does**, and a cell painted with
        /// a wrapping tap would carry a stripe of its opposite edge. The first and last destination
        /// pixels of every cell fall in this band: at eighteen across, the first tap is at
        /// 0.5 / 18 = 0.0278, and the outermost texel centre of a 256-wide tile is at 0.00195.
        TEST(MyGUIPlatformPixelsTest, aTapOutsideTheOutermostCentresHoldsTheEdge)
        {
            const osg::ref_ptr<osg::Image> image = makeGrey(4, 1, [](int x, int) { return x * 60; });

            std::uint8_t sampled[4];

            sampleBilinear(*image, 0.0f, 0.5f, sampled);
            EXPECT_EQ(sampled[0], 0) << "the left edge wrapped";

            sampleBilinear(*image, 1.0f, 0.5f, sampled);
            EXPECT_EQ(sampled[0], 180) << "the right edge wrapped";
        }

        /// The reduction the world map asks for reads four texels of the tile and no more.
        ///
        /// **Which is the whole reason this is a sampler and not an average.** A tile of alternating
        /// black and white rows carries no information at eighteen pixels across, and what the map
        /// has always shown is one row or the other rather than the grey an average gives. Rows,
        /// because a tap sits between two of them and picks up both when it is not on a centre.
        TEST(MyGUIPlatformPixelsTest, theReductionReadsFourTexelsAndNotTheWholeFootprint)
        {
            const osg::ref_ptr<osg::Image> image = makeGrey(1, 256, [](int, int y) { return y % 2 == 0 ? 0 : 255; });

            // The centre of destination pixel nine of eighteen is 9.5 / 18 of the way up, which is
            // 135.111 texels in and so 134.611 from the first texel centre: between the centres of
            // 134 and 135, 0.611 of the way across.
            std::uint8_t sampled[4];
            sampleBilinear(*image, 0.5f, 9.5f / 18.0f, sampled);

            // Texel 134 is black and 135 is white, so 0 * 0.389 + 255 * 0.611 = 155.8, which lands
            // on 156. An average of the whole fourteen-texel footprint would be 128.
            EXPECT_EQ(sampled[0], 156);
        }
        /// An image whose every pixel says where it is: red is the column, green is the row.
        ///
        /// A gather that transposed its axes, took the offset as a row count, or walked the image's
        /// own row width instead of the region's produces bytes this can name.
        osg::ref_ptr<osg::Image> makeCoordinates(int width, int height)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);

            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    std::uint8_t* pixel = image->data(x, y);
                    pixel[0] = static_cast<std::uint8_t>(x);
                    pixel[1] = static_cast<std::uint8_t>(y);
                    pixel[2] = 0;
                    pixel[3] = 255;
                }

            return image;
        }

        /// A rectangle inside a wider image comes out as its own rows, with nothing of the image
        /// between them.
        ///
        /// **This is where a region write goes wrong quietly.** The image's rows are as wide as the
        /// image, so a rectangle inside one is not a run of bytes — and a backend handed the rows
        /// with the image's stride still in them draws a sheared copy of somewhere else.
        TEST(MyGUIPlatformPixelsTest, aRegionComesOutAsItsOwnRowsAndNotSlicesOfTheImage)
        {
            const osg::ref_ptr<osg::Image> image = makeCoordinates(16, 8);

            std::vector<std::uint8_t> rows;
            gatherRegion(*image, Rect{ 3, 5, 4, 2 }, rows);

            ASSERT_EQ(rows.size(), std::size_t{ 4 } * 2 * 4) << "two rows of four pixels and nothing else";

            for (int row = 0; row < 2; ++row)
                for (int column = 0; column < 4; ++column)
                {
                    const std::uint8_t* pixel = rows.data() + (static_cast<std::size_t>(row) * 4 + column) * 4;
                    EXPECT_EQ(pixel[0], 3 + column) << "column of " << column << ", " << row;
                    EXPECT_EQ(pixel[1], 5 + row) << "row of " << column << ", " << row;
                    EXPECT_EQ(pixel[3], 255);
                }
        }

        /// The whole image is a region like any other, and the corners are where an off-by-one shows.
        TEST(MyGUIPlatformPixelsTest, theWholeImageIsARegionAndItsCornersAreWhereTheyWere)
        {
            const osg::ref_ptr<osg::Image> image = makeCoordinates(5, 3);

            std::vector<std::uint8_t> rows;
            gatherRegion(*image, Rect{ 0, 0, 5, 3 }, rows);

            ASSERT_EQ(rows.size(), std::size_t{ 5 } * 3 * 4);
            EXPECT_EQ(rows[0], 0) << "top left column";
            EXPECT_EQ(rows[1], 0) << "top left row";

            const std::uint8_t* last = rows.data() + rows.size() - 4;
            EXPECT_EQ(last[0], 4) << "bottom right column";
            EXPECT_EQ(last[1], 2) << "bottom right row";

            // And a region reused: the scratch is refilled rather than appended to.
            gatherRegion(*image, Rect{ 4, 2, 1, 1 }, rows);
            ASSERT_EQ(rows.size(), std::size_t{ 4 });
            EXPECT_EQ(rows[0], 4);
            EXPECT_EQ(rows[1], 2);
        }
    }
}
