#include <array>
#include <cstddef>
#include <string_view>

#include <gtest/gtest.h>

#include <osg/Image>

#include <components/rtx/imageformat.hpp>

namespace Rtx
{
    namespace
    {
        /// One block's worth of image in `spelling`, which is all the format reader looks at.
        osg::ref_ptr<osg::Image> makeImage(GLenum spelling)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(4, 4, 1, spelling, GL_UNSIGNED_BYTE);
            return image;
        }

        struct FormatCase
        {
            GLenum mSpelling;
            ImageFormat mFormat;
            std::string_view mName;
        };

        /// Every spelling OpenSceneGraph hands over, the format it reads as, and the name a report
        /// prints for it.
        ///
        /// **Both DXT1 spellings are one format**, because the header's alpha flag decides nothing:
        /// a BC1 block carries its punch-through bit either way.
        ///
        /// `GL_ALPHA` stands for the formats nothing here names. `ESMTerrain` builds its blend maps
        /// in it, which is a real format that reaches no uploader, so the count it lands in is the
        /// canary rather than a hole.
        TEST(RtxImageFormatTest, everySpellingReadsAsItsFormatAndNamesItself)
        {
            constexpr std::array<FormatCase, 10> sCases{ {
                { GL_COMPRESSED_RGB_S3TC_DXT1_EXT, ImageFormat::Bc1, "BC1 (DXT1)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, ImageFormat::Bc1, "BC1 (DXT1)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, ImageFormat::Bc2, "BC2 (DXT3)" },
                { GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, ImageFormat::Bc3, "BC3 (DXT5)" },
                { GL_RGB, ImageFormat::Rgb8, "RGB8" },
                { GL_RGBA, ImageFormat::Rgba8, "RGBA8" },
                { GL_BGRA, ImageFormat::Bgra8, "BGRA8" },
                { GL_LUMINANCE, ImageFormat::Luminance, "L8" },
                { GL_LUMINANCE_ALPHA, ImageFormat::LuminanceAlpha, "LA8" },
                { GL_ALPHA, ImageFormat::Unnamed, "an unnamed pixel format" },
            } };

            std::array<bool, sImageFormatCount> met{};
            for (const FormatCase& one : sCases)
            {
                EXPECT_EQ(readFormat(*makeImage(one.mSpelling)), one.mFormat);
                EXPECT_EQ(nameOf(one.mFormat), one.mName);

                met[static_cast<std::size_t>(one.mFormat)] = true;
            }

            // A format added to the enum and left out of the table above is a failure here rather
            // than a count nothing can name.
            for (std::size_t at = 0; at < met.size(); ++at)
                EXPECT_TRUE(met[at]) << "format " << at << " is in no case above";
        }
    }
}
