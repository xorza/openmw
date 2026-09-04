#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Image>

#include <components/rtx/meantexel.hpp>

namespace Rtx
{
    namespace
    {
        /// One image of four texels, in the plain spelling the sky's own decks are stored in.
        osg::ref_ptr<osg::Image> makeSheetImage(std::array<std::uint8_t, 16> bytes)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName("sheet.dds");
            image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);

            for (std::size_t at = 0; at < bytes.size(); ++at)
                image->data()[at] = bytes[at];

            return image;
        }

        /// A texel's worth of an image is its own colour in light, times how much of it is there.
        ///
        /// **Both halves are the point.** The colour is display-encoded in every format the game ships,
        /// so a mean taken before the curve is undone is the mean of the wrong quantity — and the alpha
        /// is what the sheet is drawn by, so a star sheet that is 99% transparent averages nearly
        /// nothing rather than nearly the black it is painted on.
        ///
        /// Four texels: red at full, green at half cover, blue at none, and white. Full is 1.0 in light
        /// and nought is nought, so the mean is `(1 + 0 + 0 + 1) / 4` in red, `(0 + 128/255 + 0 + 1) / 4`
        /// in green, and `(0 + 0 + 0 + 1) / 4` in blue.
        TEST(RtxMeanTexelTest, aTexelIsWorthItsColourInLightTimesHowMuchOfItIsThere)
        {
            const MeanTexel mean
                = meanTexel(*makeSheetImage({ 255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255, 255, 255 }));

            EXPECT_NEAR(mean.mColour.x(), 0.5f, 1e-5f);
            EXPECT_NEAR(mean.mColour.y(), 0.37549f, 1e-5f);
            EXPECT_NEAR(mean.mColour.z(), 0.25f, 1e-5f);

            // And the cover beside it, out of the same four: `(1 + 128/255 + 0 + 1) / 4`.
            EXPECT_NEAR(mean.mAlpha, 0.62549f, 1e-5f);
        }

        /// The curve is undone before the mean and not after it.
        ///
        /// **Mid grey is where the two answers part company.** Half of white and half of black is 128 in
        /// bytes and 0.5 in light, and those are not the same colour: 128 decodes to 0.2159. A reader
        /// that averaged the stored bytes and decoded once would say a sheet of alternating black and
        /// white is worth 0.216 where it is worth a half.
        TEST(RtxMeanTexelTest, theCurveIsUndoneBeforeTheMeanRatherThanAfterIt)
        {
            const MeanTexel chequer
                = meanTexel(*makeSheetImage({ 255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255 }));

            EXPECT_NEAR(chequer.mColour.x(), 0.5f, 1e-5f);

            const MeanTexel flat = meanTexel(
                *makeSheetImage({ 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255 }));

            EXPECT_NEAR(flat.mColour.x(), 0.21586f, 1e-5f);
        }

        /// An image in a format nothing in the game produces is one this cannot answer for.
        ///
        /// Nought rather than a throw, for the reason `readNightSky` gives about the mesh above it: the
        /// files are content, content is what a mod replaces, and a sheet nobody can average is a sheet
        /// that lights nothing rather than a renderer that will not start.
        TEST(RtxMeanTexelTest, anImageInAFormatNobodyShipsIsWorthNothing)
        {
            osg::ref_ptr<osg::Image> luminance = new osg::Image;
            luminance->setFileName("odd.dds");
            luminance->allocateImage(2, 2, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE);

            EXPECT_EQ(meanTexel(*luminance).mColour, osg::Vec3f());
            EXPECT_EQ(meanTexel(*luminance).mAlpha, 0.0f);
        }

        /// A sheet's paint is what its own alpha calls solid, and not what it adds to the sky behind it.
        ///
        /// **Which is the difference between a few wisps and a grey lid.** Two white texels at full cover
        /// beside two transparent ones average a half — the same mean a solid sheet of mid grey gives —
        /// and the two are not the same picture. Dividing the cover back out says which: the wisps come
        /// back white, because that is the colour the artist painted them.
        ///
        /// Morrowind's own clear sheet is exactly this shape, a quarter covered by cirrus.
        TEST(RtxMeanTexelTest, aSheetsPaintIsWhatItsOwnAlphaCallsSolid)
        {
            const MeanTexel wisps
                = meanTexel(*makeSheetImage({ 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0 }));

            EXPECT_NEAR(wisps.mColour.x(), 0.5f, 1e-5f);
            EXPECT_NEAR(wisps.mAlpha, 0.5f, 1e-5f);
            EXPECT_NEAR(wisps.opaque().x(), 1.0f, 1e-5f);

            // And a sheet with nothing painted on it has no paint to average, rather than a division by
            // the nothing that covers it.
            const MeanTexel empty = meanTexel(
                *makeSheetImage({ 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0 }));

            EXPECT_EQ(empty.mAlpha, 0.0f);
            EXPECT_EQ(empty.opaque(), osg::Vec3f());
        }
    }
}
