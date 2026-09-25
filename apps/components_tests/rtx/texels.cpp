#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include <osg/GL>
#include <osg/Image>
#include <osg/Texture>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/rtx/meantexels.hpp>
#include <components/rtx/texels.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/textureencoding.hpp>

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

        /// A loose texel is read in the order its format states the colours, in either encoding: a
        /// BGRA8 texel of bytes `(10, 20, 30, 255)` is red 30 and blue 10, linear or display-encoded.
        TEST(RtxTexelTest, aLooseTexelIsReadInItsFormatsOrderInEitherEncoding)
        {
            const std::array<std::byte, 4> bytes{ std::byte{ 10 }, std::byte{ 20 }, std::byte{ 30 }, std::byte{ 255 } };
            const std::array levels{ MipLevel{ 0, 1, 1 } };

            for (const TextureFormat format : { TextureFormat::Bgra8Srgb, TextureFormat::Bgra8Unorm })
            {
                const TextureData texture{
                    .mFormat = format, .mWidth = 1, .mHeight = 1, .mBytes = bytes, .mLevels = levels
                };
                EXPECT_EQ(texelAt(texture, levels[0], 0, 0), osg::Vec3f(30 / 255.0f, 20 / 255.0f, 10 / 255.0f))
                    << nameOf(format);
            }

            const TextureData rgba{
                .mFormat = TextureFormat::Rgba8Unorm, .mWidth = 1, .mHeight = 1, .mBytes = bytes, .mLevels = levels
            };
            EXPECT_EQ(texelAt(rgba, levels[0], 0, 0), osg::Vec3f(10 / 255.0f, 20 / 255.0f, 30 / 255.0f));
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
        /// in green, and `(0 + 0 + 0 + 1) / 4` in blue. With the alpha unread every channel is
        /// `(1 + 1) / 4`, which is what a sheet that adds whole is worth.
        TEST(RtxMeanTexelTest, aTexelIsWorthItsColourInLightTimesHowMuchOfItIsThere)
        {
            const MeanTexel mean
                = meanTexel(*makeSheetImage({ 255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255, 255, 255 }));

            EXPECT_NEAR(mean.mColour.x(), 0.5f, 1e-5f);
            EXPECT_NEAR(mean.mColour.y(), 0.37549f, 1e-5f);
            EXPECT_NEAR(mean.mColour.z(), 0.25f, 1e-5f);

            EXPECT_NEAR(mean.mWhole.x(), 0.5f, 1e-5f);
            EXPECT_NEAR(mean.mWhole.y(), 0.5f, 1e-5f);
            EXPECT_NEAR(mean.mWhole.z(), 0.5f, 1e-5f);

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

        /// A file is averaged once for the process and found by its name after: two images of one
        /// file, in two spellings of it, are one entry and one reference, and a second ask reads
        /// nothing — the entry stands where it stood. An image that is not a file is averaged at
        /// every ask and kept nowhere.
        TEST(RtxMeanTexelsTest, aFileIsAveragedOnceAndFoundByItsName)
        {
            MeanTexels means;

            osg::ref_ptr<osg::Image> red
                = makeSheetImage({ 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255 });
            red->setFileName("Textures\\VFX_Fire.dds");
            const MeanTexel& first = means.of(*red);
            EXPECT_NEAR(first.mColour.x(), 1.0f, 1e-5f);
            EXPECT_EQ(means.size(), 1u);

            // The same file spelt the way the texture table spells it, and painted differently:
            // the cache answers for the name and never reads the second image.
            osg::ref_ptr<osg::Image> again
                = makeSheetImage({ 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255 });
            again->setFileName("textures/vfx_fire.dds");
            const MeanTexel& second = means.of(*again);
            EXPECT_EQ(&second, &first) << "a second spelling of one file made a second entry";
            EXPECT_NEAR(second.mColour.x(), 1.0f, 1e-5f) << "the second image was read";
            EXPECT_EQ(means.size(), 1u);

            osg::ref_ptr<osg::Image> unnamed
                = makeSheetImage({ 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255 });
            unnamed->setFileName("");
            EXPECT_NEAR(means.of(*unnamed).mColour.y(), 1.0f, 1e-5f);
            EXPECT_EQ(means.size(), 1u) << "an unnamed image was kept";
            EXPECT_EQ(&means.of(*red), &first) << "an unnamed ask moved a file's entry";
        }
    }

    namespace
    {
        /// One block's worth of image in `spelling` and `type`, stated as `internal` where that is
        /// not nought, which is all the format reader looks at.
        osg::ref_ptr<osg::Image> makeImage(GLenum spelling, GLenum type = GL_UNSIGNED_BYTE, GLint internal = 0)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(4, 4, 1, spelling, type);
            if (internal != 0)
                image->setInternalTextureFormat(internal);
            return image;
        }

        struct FormatCase
        {
            GLenum mSpelling;
            TextureEncoding mEncoding;
            TextureFormat mFormat;
            std::string_view mName;
            GLenum mType = GL_UNSIGNED_BYTE;
            GLint mInternal = 0;
        };

        /// Every spelling OpenSceneGraph hands over, the format it reads as under each encoding, and
        /// the name a report prints for it.
        ///
        /// **Both DXT1 spellings are one format**, because the header's alpha flag decides nothing:
        /// a BC1 block carries its punch-through bit either way.
        ///
        /// **Data is the same blocks without the curve, and two channels are data alone**: a BC5
        /// file bound as a colour has lost its blue, and is no format a colour slot takes.
        ///
        /// `GL_ALPHA` stands for the formats nothing here names. `ESMTerrain` builds its blend maps
        /// in it, which is a real format that reaches no uploader, so the count it lands in is the
        /// canary rather than a hole.
        ///
        /// **A loose format is its pixel format and its data type together.** OpenSceneGraph's DDS
        /// loader hands an A1R5G5B5 file over as `GL_BGRA` of `GL_UNSIGNED_SHORT_1_5_5_5_REV`, and
        /// read by its pixel format alone it was BGRA8 — four bytes a texel for a file of two. The
        /// sixteen-bit spellings the loader makes are named, the `X` ones by the internal format it
        /// states them in; every other data type under a loose pixel format is unnamed.
        TEST(RtxTextureFormatTest, everySpellingReadsAsItsFormatAndNamesItself)
        {
            using enum TextureEncoding;
            constexpr std::array<FormatCase, 30> sCases{ {
                { GL_COMPRESSED_RGB_S3TC_DXT1_EXT, Colour, TextureFormat::Bc1RgbaSrgb, "BC1 (DXT1)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, Colour, TextureFormat::Bc1RgbaSrgb, "BC1 (DXT1)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, Colour, TextureFormat::Bc2Srgb, "BC2 (DXT3)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, Colour, TextureFormat::Bc3Srgb, "BC3 (DXT5)" },
                { GL_COMPRESSED_RED_GREEN_RGTC2_EXT, Colour, TextureFormat::Unnamed, "an unnamed pixel format" },
                { GL_RGB, Colour, TextureFormat::Rgb8, "RGB8" },
                { GL_RGBA, Colour, TextureFormat::Rgba8Srgb, "RGBA8" },
                { GL_BGRA, Colour, TextureFormat::Bgra8Srgb, "BGRA8" },
                { GL_LUMINANCE, Colour, TextureFormat::Luminance, "L8" },
                { GL_LUMINANCE_ALPHA, Colour, TextureFormat::LuminanceAlpha, "LA8" },
                { GL_ALPHA, Colour, TextureFormat::Unnamed, "an unnamed pixel format" },
                { GL_COMPRESSED_RGB_S3TC_DXT1_EXT, Data, TextureFormat::Bc1RgbaUnorm, "BC1 (DXT1, linear)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, Data, TextureFormat::Bc1RgbaUnorm, "BC1 (DXT1, linear)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, Data, TextureFormat::Bc2Unorm, "BC2 (DXT3, linear)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, Data, TextureFormat::Bc3Unorm, "BC3 (DXT5, linear)" },
                { GL_COMPRESSED_RED_GREEN_RGTC2_EXT, Data, TextureFormat::Bc5Unorm, "BC5 (ATI2, linear)" },
                { GL_RGBA, Data, TextureFormat::Rgba8Unorm, "RGBA8 (linear)" },
                { GL_BGRA, Data, TextureFormat::Bgra8Unorm, "BGRA8 (linear)" },
                { GL_RGB, Data, TextureFormat::Rgb8, "RGB8" },
                { GL_RGB, Colour, TextureFormat::Rgb565, "R5G6B5", GL_UNSIGNED_SHORT_5_6_5 },
                { GL_BGRA, Colour, TextureFormat::Argb1555, "A1R5G5B5", GL_UNSIGNED_SHORT_1_5_5_5_REV },
                { GL_BGRA, Colour, TextureFormat::Xrgb1555, "X1R5G5B5", GL_UNSIGNED_SHORT_1_5_5_5_REV, GL_RGB },
                { GL_BGRA, Data, TextureFormat::Argb4444, "A4R4G4B4", GL_UNSIGNED_SHORT_4_4_4_4_REV },
                { GL_BGRA, Colour, TextureFormat::Xrgb4444, "X4R4G4B4", GL_UNSIGNED_SHORT_4_4_4_4_REV, GL_RGB },
                { GL_RGBA, Colour, TextureFormat::Unnamed, "an unnamed pixel format", GL_UNSIGNED_SHORT_4_4_4_4 },
                { GL_BGRA, Colour, TextureFormat::Unnamed, "an unnamed pixel format", GL_UNSIGNED_INT_2_10_10_10_REV },
                { GL_RGBA, Data, TextureFormat::Unnamed, "an unnamed pixel format", GL_UNSIGNED_SHORT },
                { GL_RGBA, Colour, TextureFormat::Unnamed, "an unnamed pixel format", GL_HALF_FLOAT },
                { GL_LUMINANCE, Colour, TextureFormat::Unnamed, "an unnamed pixel format", GL_UNSIGNED_SHORT },
                { GL_RGB, Colour, TextureFormat::Unnamed, "an unnamed pixel format", GL_UNSIGNED_BYTE_3_3_2 },
            } };

            std::array<bool, sTextureFormatCount> met{};
            for (const FormatCase& one : sCases)
            {
                EXPECT_EQ(readFormat(*makeImage(one.mSpelling, one.mType, one.mInternal), one.mEncoding), one.mFormat)
                    << one.mName << " of type " << one.mType;
                EXPECT_EQ(nameOf(one.mFormat), one.mName);
                EXPECT_EQ(isUploadable(one.mFormat), one.mFormat < TextureFormat::Rgb565) << one.mName;
                EXPECT_EQ(
                    isWidened(one.mFormat), one.mFormat >= TextureFormat::Rgb565 && one.mFormat < TextureFormat::Rgb8)
                    << one.mName;
                EXPECT_EQ(isSrgb(one.mFormat), one.mEncoding == Colour && isUploadable(one.mFormat)) << one.mName;
                EXPECT_EQ(isBc1(one.mFormat),
                    one.mSpelling == GL_COMPRESSED_RGB_S3TC_DXT1_EXT
                        || one.mSpelling == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT)
                    << one.mName;

                met[static_cast<std::size_t>(one.mFormat)] = true;
            }

            // Read with no encoding named, a file is a colour: every caller but a companion map's.
            EXPECT_EQ(readFormat(*makeImage(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT)), TextureFormat::Bc3Srgb);

            // A format added to the enum and left out of the table above is a failure here rather
            // than a count nothing can name.
            for (std::size_t at = 0; at < met.size(); ++at)
                EXPECT_TRUE(met[at]) << "format " << at << " is in no case above";
        }
    }
}
