#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Image>

#include <components/myguiplatform/pixels.hpp>

namespace MyGUIPlatform
{
    namespace
    {
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
            gatherRegion(*image, 3, 5, 4, 2, rows);

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
            gatherRegion(*image, 0, 0, 5, 3, rows);

            ASSERT_EQ(rows.size(), std::size_t{ 5 } * 3 * 4);
            EXPECT_EQ(rows[0], 0) << "top left column";
            EXPECT_EQ(rows[1], 0) << "top left row";

            const std::uint8_t* last = rows.data() + rows.size() - 4;
            EXPECT_EQ(last[0], 4) << "bottom right column";
            EXPECT_EQ(last[1], 2) << "bottom right row";

            // And a region reused: the scratch is refilled rather than appended to.
            gatherRegion(*image, 4, 2, 1, 1, rows);
            ASSERT_EQ(rows.size(), std::size_t{ 4 });
            EXPECT_EQ(rows[0], 4);
            EXPECT_EQ(rows[1], 2);
        }
    }
}
