#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Texture2D>

#include <components/myguiplatform/myguitexture.hpp>

namespace MyGUIPlatform
{
    namespace
    {
        /// A four-by-four picture the GUI asked for as RGBA, which is what a region write needs.
        ///
        /// **No GL context anywhere here.** What the upload does with a run of rows is the driver's;
        /// what is worth asserting is which pixels the picture holds and which rows are owed, and
        /// neither of those touches one.
        std::uint32_t makeTexture(OSGTexture& texture, int side)
        {
            texture.createManual(
                side, side, MyGUI::TextureUsage::Static | MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);

            return static_cast<std::uint32_t>(side) * side * 4;
        }

        /// Fills the whole picture with `colour` through a lock, the way `Picture::set` does.
        void fill(OSGTexture& texture, std::array<std::uint8_t, 4> colour)
        {
            auto* pixels = static_cast<std::uint8_t*>(texture.lock(MyGUI::TextureUsage::Write));
            for (int at = 0; at < texture.getWidth() * texture.getHeight(); ++at)
                std::copy(colour.begin(), colour.end(), pixels + at * 4);

            texture.unlock();
        }

        /// The four bytes at a pixel, read back out of the buffer `lock` hands out.
        std::array<std::uint8_t, 4> at(OSGTexture& texture, int x, int y)
        {
            const auto* pixels = static_cast<const std::uint8_t*>(texture.lock(MyGUI::TextureUsage::Write));
            const std::size_t offset = (static_cast<std::size_t>(y) * texture.getWidth() + x) * 4;
            const std::array<std::uint8_t, 4> texel{ pixels[offset], pixels[offset + 1], pixels[offset + 2],
                pixels[offset + 3] };

            texture.unlock();
            return texel;
        }

        /// The buffer is the texture's own picture, not one made for the lock that asked.
        ///
        /// **This is the whole of what a video frame costs now.** MyGUI hands out a buffer and takes
        /// it back filled; a fresh one each time is a megabyte allocated and a megabyte freed per
        /// frame, and the picture the caller wrote last time is gone — which is what makes writing
        /// part of one impossible.
        TEST(MyGUIPlatformTextureTest, theLockedBufferIsTheSameOneAndKeepsWhatWasWrittenIntoIt)
        {
            OSGTexture texture("picture", nullptr);
            makeTexture(texture, 4);

            void* first = texture.lock(MyGUI::TextureUsage::Write);
            static_cast<std::uint8_t*>(first)[0] = 0xAB;
            texture.unlock();

            void* second = texture.lock(MyGUI::TextureUsage::Write);
            EXPECT_EQ(first, second) << "a lock allocated a buffer of its own";
            EXPECT_EQ(static_cast<std::uint8_t*>(second)[0], 0xAB) << "what the last write left is gone";
            texture.unlock();
        }

        /// A region write lands on its own rectangle and leaves every pixel outside it alone.
        TEST(MyGUIPlatformTextureTest, aRegionWriteChangesItsRectangleAndNothingElse)
        {
            constexpr int side = 4;

            OSGTexture texture("picture", nullptr);
            makeTexture(texture, side);
            fill(texture, { 255, 0, 0, 255 });

            // Two texels wide and one tall, at the second column of the third row: a write that
            // ignored the offset, took it as a row count, or transposed it lands where the sweep
            // below looks.
            constexpr std::array<std::uint8_t, 8> green{ 0, 255, 0, 255, 0, 255, 0, 255 };
            texture.writeRegion(1, 2, 2, 1, green);

            for (int row = 0; row < side; ++row)
                for (int column = 0; column < side; ++column)
                {
                    const bool written = row == 2 && (column == 1 || column == 2);
                    const std::array<std::uint8_t, 4> expected = written
                        ? std::array<std::uint8_t, 4>{ 0, 255, 0, 255 }
                        : std::array<std::uint8_t, 4>{ 255, 0, 0, 255 };

                    EXPECT_EQ(at(texture, column, row), expected)
                        << "texel " << column << ", " << row << (written ? " was not written" : " was not left alone");
                }
        }

        /// What is owed to the device is the rows that were written, and their union where several
        /// were.
        ///
        /// **Rows and not rectangles**, which is what makes a run one span of the picture: the map
        /// writes a block eighteen pixels square when a cell arrives, and eighteen rows of it is a
        /// twentieth of the surface where the whole surface was what used to go.
        TEST(MyGUIPlatformTextureTest, theRowsOwedAreTheRowsWrittenAndTheirUnion)
        {
            constexpr int side = 8;

            OSGTexture texture("picture", nullptr);
            makeTexture(texture, side);

            EXPECT_TRUE(texture.getDirtyRows().empty()) << "a picture nothing has written owes rows";

            constexpr std::array<std::uint8_t, 4> texel{ 1, 2, 3, 4 };
            texture.writeRegion(0, 5, 1, 1, texel);

            EXPECT_EQ(texture.getDirtyRows().mFirst, 5);
            EXPECT_EQ(texture.getDirtyRows().mCount, 1);

            // Above it and not touching, so the union has to reach across the gap: one run is what
            // an upload is, and two disjoint ones would be two.
            texture.writeRegion(0, 1, 1, 1, texel);

            EXPECT_EQ(texture.getDirtyRows().mFirst, 1);
            EXPECT_EQ(texture.getDirtyRows().mCount, 5) << "rows 1 through 5";

            // Inside what is already owed, which changes nothing.
            texture.writeRegion(0, 3, 1, 1, texel);

            EXPECT_EQ(texture.getDirtyRows().mFirst, 1);
            EXPECT_EQ(texture.getDirtyRows().mCount, 5);

            // And a whole-surface write is every row of it.
            fill(texture, { 0, 0, 0, 255 });

            EXPECT_EQ(texture.getDirtyRows().mFirst, 0);
            EXPECT_EQ(texture.getDirtyRows().mCount, side);
        }

        /// The picture is copied once, on the first write after the interface has drawn with it.
        ///
        /// **Which is the whole of what makes writing in place safe.** The draw traversal may run
        /// beside the next frame's update, so a picture that has been batched cannot be overwritten
        /// where it lies — the copy is what the draw goes on reading, and `DYNAMIC` on the one that
        /// replaces it is what stops the two traversals overlapping on it again.
        TEST(MyGUIPlatformTextureTest, thePictureIsCopiedOnceOnTheFirstWriteAfterItHasBeenDrawn)
        {
            OSGTexture texture("picture", nullptr);
            makeTexture(texture, 4);

            const osg::Texture2D* made = texture.getTexture();
            ASSERT_NE(made, nullptr);
            EXPECT_FALSE(texture.isDynamic());

            // Before it has been drawn with, nothing can be reading it: written where it lies,
            // however often. A font atlas is filled here and never leaves.
            fill(texture, { 1, 1, 1, 255 });
            fill(texture, { 2, 2, 2, 255 });

            EXPECT_EQ(texture.getTexture(), made) << "a texture nothing has drawn with was copied";
            EXPECT_FALSE(texture.isDynamic());

            texture.markDrawn();

            fill(texture, { 3, 3, 3, 255 });

            const osg::Texture2D* promoted = texture.getTexture();
            EXPECT_NE(promoted, made) << "the picture the draw traversal is reading was written over";
            EXPECT_TRUE(texture.isDynamic());
            EXPECT_EQ(promoted->getDataVariance(), osg::Object::DYNAMIC)
                << "the drawable is never told, so the two traversals go on overlapping";

            // What was in it survived the copy.
            EXPECT_EQ(at(texture, 0, 0), (std::array<std::uint8_t, 4>{ 3, 3, 3, 255 }));

            // And once, not once per write: the copy is what `DYNAMIC` has made safe to write.
            fill(texture, { 4, 4, 4, 255 });
            texture.writeRegion(0, 0, 1, 1, std::array<std::uint8_t, 4>{ 5, 5, 5, 255 });

            EXPECT_EQ(texture.getTexture(), promoted) << "a dynamic texture was copied again";
            EXPECT_EQ(at(texture, 0, 0), (std::array<std::uint8_t, 4>{ 5, 5, 5, 255 }));
        }

        /// Making the picture again starts it over, drawn with or not.
        ///
        /// `Picture::set` remakes a texture whenever the image changes shape, and what comes out is a
        /// texture object nothing has ever batched — so the copy that a written-over picture needs is
        /// one this must not go on paying.
        TEST(MyGUIPlatformTextureTest, makingThePictureAgainStartsItOver)
        {
            OSGTexture texture("picture", nullptr);
            makeTexture(texture, 4);

            texture.markDrawn();
            fill(texture, { 1, 1, 1, 255 });
            ASSERT_TRUE(texture.isDynamic());

            makeTexture(texture, 8);

            EXPECT_FALSE(texture.isDynamic());
            EXPECT_EQ(texture.getWidth(), 8);

            const osg::Texture2D* made = texture.getTexture();
            fill(texture, { 2, 2, 2, 255 });

            EXPECT_EQ(texture.getTexture(), made) << "a texture nothing has drawn with was copied";
        }
    }
}
