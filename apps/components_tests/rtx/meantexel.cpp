#include <components/rtx/meantexel.hpp>

#include <gtest/gtest.h>

#include <osg/Image>

#include <array>
#include <cstdint>

namespace
{
    /// One image of four texels, in the plain spelling the sky's own decks are stored in.
    osg::ref_ptr<osg::Image> makeSheet(std::array<std::uint8_t, 16> bytes)
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
        const osg::Vec3f mean
            = Rtx::meanTexel(*makeSheet({ 255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255, 255, 255 }));

        EXPECT_NEAR(mean.x(), 0.5f, 1e-5f);
        EXPECT_NEAR(mean.y(), 0.37549f, 1e-5f);
        EXPECT_NEAR(mean.z(), 0.25f, 1e-5f);
    }

    /// The curve is undone before the mean and not after it.
    ///
    /// **Mid grey is where the two answers part company.** Half of white and half of black is 128 in
    /// bytes and 0.5 in light, and those are not the same colour: 128 decodes to 0.2159. A reader
    /// that averaged the stored bytes and decoded once would say a sheet of alternating black and
    /// white is worth 0.216 where it is worth a half.
    TEST(RtxMeanTexelTest, theCurveIsUndoneBeforeTheMeanRatherThanAfterIt)
    {
        const osg::Vec3f chequer
            = Rtx::meanTexel(*makeSheet({ 255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255 }));

        EXPECT_NEAR(chequer.x(), 0.5f, 1e-5f);

        const osg::Vec3f flat = Rtx::meanTexel(
            *makeSheet({ 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255 }));

        EXPECT_NEAR(flat.x(), 0.21586f, 1e-5f);
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

        EXPECT_EQ(Rtx::meanTexel(*luminance), osg::Vec3f());
    }
}
