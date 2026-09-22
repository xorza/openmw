#include <gtest/gtest.h>

#include <osg/Image>
#include <osg/ref_ptr>

#include <components/sceneutil/paintedtexture.hpp>

namespace SceneUtil
{
    namespace
    {
        /// A picture sixteen texels wide and eight high, RGBA8 as both painters allocate it.
        osg::ref_ptr<osg::Image> picture()
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(16, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            return image;
        }

        /// Two rectangles join into the smallest one holding both: x from 2 to 7, y from 1 to 11.
        /// An empty one joins as the other.
        TEST(SceneUtilPaintedTextureTest, aJoinIsTheSmallestRegionHoldingBoth)
        {
            const ImageRegion a{ 2, 3, 4, 5 };
            const ImageRegion b{ 5, 1, 2, 10 };
            const ImageRegion both{ 2, 1, 5, 10 };

            EXPECT_EQ(a.joined(b), both);
            EXPECT_EQ(b.joined(a), both);
            EXPECT_EQ(a.joined(ImageRegion{}), a);
            EXPECT_EQ(ImageRegion{}.joined(b), b);
            EXPECT_TRUE(ImageRegion{}.joined(ImageRegion{}).empty());
        }

        /// Each reader is answered from its own count: nothing where nothing was painted after
        /// it, the union of what was otherwise, and every answer carries the count to remember.
        TEST(SceneUtilPaintedTextureTest, aReaderIsHandedWhatWasPaintedSinceItsOwnCount)
        {
            osg::ref_ptr<osg::Image> image = picture();
            osg::ref_ptr<PaintedTexture> texture = new PaintedTexture(image);
            const unsigned int unpainted = image->getModifiedCount();

            EXPECT_TRUE(texture->since(0).mRegion.empty()) << "nothing painted yet";
            EXPECT_EQ(texture->since(0).mPaints, 0u);

            const ImageRegion first{ 1, 1, 2, 2 };
            const ImageRegion second{ 10, 4, 3, 3 };
            texture->paint(first);
            texture->paint(second);

            // The rasterizer's own reader: no callback of its own, and the image dirtied once a
            // paint, so the texture sends the whole of it on its next apply as upstream sends the
            // fog.
            EXPECT_EQ(texture->getSubloadCallback(), nullptr);
            EXPECT_EQ(image->getModifiedCount(), unpainted + 2);

            const Painted fromStart = texture->since(0);
            EXPECT_EQ(fromStart.mRegion, first.joined(second));
            EXPECT_EQ(fromStart.mPaints, 2u);

            EXPECT_EQ(texture->since(1).mRegion, second) << "a reader that had the first wants the second";
            EXPECT_TRUE(texture->since(2).mRegion.empty()) << "and one that had both wants nothing";

            texture->paint(ImageRegion{});
            EXPECT_EQ(texture->since(2).mPaints, 2u) << "an empty paint is no paint";
            EXPECT_EQ(image->getModifiedCount(), unpainted + 2) << "and dirties nothing";
        }

        /// A reader that fell further behind than the texture remembers is handed the whole
        /// picture, and one within reach the union of only what it missed.
        TEST(SceneUtilPaintedTextureTest, aReaderTooFarBehindIsHandedTheWholePicture)
        {
            osg::ref_ptr<osg::Image> image = picture();
            osg::ref_ptr<PaintedTexture> texture = new PaintedTexture(image);

            // Nine one-texel paints along the bottom row, at x = 0 to 8.
            for (int x = 0; x < 9; ++x)
                texture->paint(ImageRegion{ x, 0, 1, 1 });

            EXPECT_EQ(texture->since(0).mRegion, texture->whole()) << "nine back is past the eight remembered";
            EXPECT_EQ(texture->since(1).mRegion, (ImageRegion{ 1, 0, 8, 1 })) << "eight back is the last eight";
            EXPECT_EQ(texture->since(7).mRegion, (ImageRegion{ 7, 0, 2, 1 }));
            EXPECT_EQ(texture->since(9).mPaints, 9u);

            texture->paintAll();
            EXPECT_EQ(texture->since(9).mRegion, (ImageRegion{ 0, 0, 16, 8 }));
        }
    }
}
