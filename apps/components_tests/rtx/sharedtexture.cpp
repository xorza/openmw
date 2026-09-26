#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/GL>
#include <osg/Image>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

#include <components/myguirtx/sharedtexture.hpp>
#include <components/rtx/guirenderer.hpp>
#include <components/rtx/slot.hpp>

#include "support/countingrenderer.hpp"

namespace MyGUIRtx
{
    namespace
    {
        /// A packed RGBA picture whose bytes count up from `first`, so every byte sent says which
        /// picture and which place it came from.
        osg::ref_ptr<osg::Image> makePicture(const int width, const int height, const std::uint8_t first)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            for (unsigned int at = 0; at < image->getTotalSizeInBytes(); ++at)
                image->data()[at] = static_cast<std::uint8_t>(first + at);
            return image;
        }

        std::vector<std::uint8_t> bytesOf(const osg::Image& image)
        {
            return std::vector<std::uint8_t>(image.data(), image.data() + image.getTotalSizeInBytes());
        }

        std::array<std::uint32_t, 4> cornersOf(const Rtx::GuiRegion& region)
        {
            return { region.mX, region.mY, region.mWidth, region.mHeight };
        }

        /// **Whole, whenever the picture moves, and nothing when it does not.** A row written in
        /// place goes as the whole picture and not as that row, which is what says no comparison
        /// against what was sent is left; another picture of the same shape keeps the slot, and one
        /// of another shape takes a new one.
        TEST(RtxSharedTextureTest, aMirrorSendsItsPictureWholeWheneverThePictureMoves)
        {
            Rtx::Testing::CountingRenderer renderer;
            const osg::ref_ptr<osg::Image> first = makePicture(4, 3, 0);
            const osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(first);

            SharedTexture mirror(renderer, *texture);
            ASSERT_EQ(renderer.mLent.size(), 1u);
            EXPECT_EQ(cornersOf(renderer.mLent[0]), (std::array<std::uint32_t, 4>{ 0, 0, 4, 3 }));
            EXPECT_EQ(renderer.mLending, bytesOf(*first));
            EXPECT_EQ(renderer.mGuiSent, 1u);
            const Rtx::GuiSlot slot = mirror.getSlot();

            mirror.refresh();
            EXPECT_EQ(renderer.mLent.size(), 1u);
            EXPECT_EQ(renderer.mGuiSent, 1u);

            // The second of three rows, 16 bytes in: what a comparison would have sent alone.
            for (int x = 0; x < 16; ++x)
                first->data(0, 1)[x] = 200;
            first->dirty();
            mirror.refresh();
            ASSERT_EQ(renderer.mLent.size(), 2u);
            EXPECT_EQ(cornersOf(renderer.mLent[1]), (std::array<std::uint32_t, 4>{ 0, 0, 4, 3 }));
            EXPECT_EQ(renderer.mLending, bytesOf(*first));
            EXPECT_EQ(renderer.mGuiSent, 2u);

            const osg::ref_ptr<osg::Image> second = makePicture(4, 3, 100);
            texture->setImage(second);
            mirror.refresh();
            ASSERT_EQ(renderer.mLent.size(), 3u);
            EXPECT_EQ(cornersOf(renderer.mLent[2]), (std::array<std::uint32_t, 4>{ 0, 0, 4, 3 }));
            EXPECT_EQ(renderer.mLending, bytesOf(*second));
            EXPECT_EQ(mirror.getSlot(), slot);

            const osg::ref_ptr<osg::Image> smaller = makePicture(2, 2, 50);
            texture->setImage(smaller);
            mirror.refresh();
            ASSERT_EQ(renderer.mLent.size(), 4u);
            EXPECT_EQ(cornersOf(renderer.mLent[3]), (std::array<std::uint32_t, 4>{ 0, 0, 2, 2 }));
            EXPECT_EQ(renderer.mLending, bytesOf(*smaller));
            EXPECT_NE(mirror.getSlot(), slot);
            EXPECT_EQ(renderer.mGuiSent, 4u);

            // Three channels, as a save's thumbnail is: each byte read as n / 255 and written back
            // as n / 255 * 255 + 0.5 rounded down, which is n, beside an opaque alpha.
            const osg::ref_ptr<osg::Image> rgb = new osg::Image;
            rgb->allocateImage(2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE);
            for (unsigned int at = 0; at < 6; ++at)
                rgb->data()[at] = static_cast<std::uint8_t>(10 * (at + 1));
            texture->setImage(rgb);
            mirror.refresh();
            EXPECT_EQ(renderer.mLending, (std::vector<std::uint8_t>{ 10, 20, 30, 255, 40, 50, 60, 255 }));
        }

        /// **The picture last sent lives until the mirror has seen the next one.** One the game let
        /// go of could otherwise be followed by another at its address with the same modified count,
        /// and the mirror would take the new picture for the one it already sent.
        TEST(RtxSharedTextureTest, aMirrorHoldsThePictureItLastSentUntilItSeesTheNext)
        {
            Rtx::Testing::CountingRenderer renderer;
            osg::ref_ptr<osg::Image> first = makePicture(4, 3, 0);
            const osg::observer_ptr<osg::Image> watched(first);
            const osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(first);
            SharedTexture mirror(renderer, *texture);

            texture->setImage(makePicture(4, 3, 100));
            first = nullptr;
            EXPECT_TRUE(watched.valid());

            mirror.refresh();
            EXPECT_FALSE(watched.valid());
        }
    }
}
