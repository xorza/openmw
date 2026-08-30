#include "imageformat.hpp"

#include <osg/Image>

namespace Rtx
{
    ImageFormat readFormat(const osg::Image& image)
    {
        switch (image.getPixelFormat())
        {
            // One format for both spellings: whether the file's header claimed alpha decides
            // nothing, since a BC1 block carries its punch-through bit either way.
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
                return ImageFormat::Bc1;
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
                return ImageFormat::Bc2;
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                return ImageFormat::Bc3;
            case GL_RGB:
                return ImageFormat::Rgb8;
            case GL_RGBA:
                return ImageFormat::Rgba8;
            case GL_BGRA:
                return ImageFormat::Bgra8;
            case GL_LUMINANCE:
                return ImageFormat::Luminance;
            case GL_LUMINANCE_ALPHA:
                return ImageFormat::LuminanceAlpha;
            default:
                return ImageFormat::Unnamed;
        }
    }

    std::string_view nameOf(ImageFormat format)
    {
        switch (format)
        {
            case ImageFormat::Bc1:
                return "BC1 (DXT1)";
            case ImageFormat::Bc2:
                return "BC2 (DXT3)";
            case ImageFormat::Bc3:
                return "BC3 (DXT5)";
            case ImageFormat::Rgb8:
                return "RGB8";
            case ImageFormat::Rgba8:
                return "RGBA8";
            case ImageFormat::Bgra8:
                return "BGRA8";
            case ImageFormat::Luminance:
                return "L8";
            case ImageFormat::LuminanceAlpha:
                return "LA8";
            case ImageFormat::Unnamed:
                break;
        }

        return "an unnamed pixel format";
    }
}
