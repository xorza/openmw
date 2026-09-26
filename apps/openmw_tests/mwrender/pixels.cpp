#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include <osg/GL>
#include <osg/Image>
#include <osg/ref_ptr>

#include <apps/openmw/mwrender/pixels.hpp>
#include <components/sceneutil/imageregion.hpp>

namespace MWRender
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
        TEST(MWRenderPixelsTest, aTapBetweenFourTexelsWeighsThemByDistance)
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
        TEST(MWRenderPixelsTest, aTapOnATexelCentreIsThatTexel)
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
        TEST(MWRenderPixelsTest, aTapBetweenTwoCentresIsTheirMean)
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
        TEST(MWRenderPixelsTest, aTapOutsideTheOutermostCentresHoldsTheEdge)
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
        TEST(MWRenderPixelsTest, theReductionReadsFourTexelsAndNotTheWholeFootprint)
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

        /// **An explored cell paints its tile through the land alpha, and only where it changed.**
        ///
        /// A two-by-two tile lands on a two-by-two cell of the overlay one texel to a pixel, so each
        /// pixel is its own texel: (0.5 / 2) * 2 - 0.5 = 0 is dead on texel nought. The land alpha
        /// is land on the left column and sea on the right, so the right column's alpha goes to
        /// nought and its colour stays. Painted again, nothing changed and the caller is told so.
        TEST(MWRenderPixelsTest, aTilePaintsThroughTheLandAlphaAndReportsAChange)
        {
            const osg::ref_ptr<osg::Image> tile = makeGrey(2, 2, [](int x, int y) { return 50 + x * 100 + y * 20; });

            osg::ref_ptr<osg::Image> land = new osg::Image;
            land->allocateImage(4, 4, 1, GL_ALPHA, GL_UNSIGNED_BYTE);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    *land->data(x, y) = x < 3 ? 255 : 0;

            const osg::ref_ptr<osg::Image> overlay = makeGrey(4, 4, [](int, int) { return 0; });
            std::vector<std::uint8_t> scratch;

            // The cell at (2, 1): its right column is x = 3, which is sea.
            const SceneUtil::ImageRegion cell{ 2, 1, 2, 2 };
            EXPECT_TRUE(compositeTile(*tile, *land, *overlay, cell, scratch));

            const std::uint8_t* lowerLeft = overlay->data(2, 1);
            EXPECT_EQ(lowerLeft[0], 50);
            EXPECT_EQ(lowerLeft[3], 50) << "land keeps the tile's alpha";
            const std::uint8_t* lowerRight = overlay->data(3, 1);
            EXPECT_EQ(lowerRight[0], 150) << "the colour stays";
            EXPECT_EQ(lowerRight[3], 0) << "sea takes the alpha";
            const std::uint8_t* upperLeft = overlay->data(2, 2);
            EXPECT_EQ(upperLeft[0], 70);
            EXPECT_EQ(upperLeft[3], 70);
            EXPECT_EQ(overlay->data(1, 1)[0], 0) << "nothing outside the cell";
            EXPECT_EQ(overlay->data(2, 3)[0], 0) << "nothing outside the cell";

            EXPECT_FALSE(compositeTile(*tile, *land, *overlay, cell, scratch)) << "the same tile again changes nothing";
        }
    }
}
