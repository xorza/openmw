#include "texels.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <osg/Image>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>

#include <components/debug/debuglog.hpp>
#include <components/files/conversion.hpp>

#include "alphaimage.hpp"
#include "colour.hpp"
#include "error.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    osg::Vec3f texelAt(const TextureData& texture, const MipLevel& level, std::uint32_t x, std::uint32_t y)
    {
        assert(x < level.mWidth && y < level.mHeight);

        const std::uint32_t bytes = blockBytes(texture.mFormat);
        if (bytes == 0)
        {
            const std::size_t at = level.mOffset + (std::size_t{ y } * level.mWidth + x) * 4;
            const auto channel = [&](std::size_t offset) {
                return std::to_integer<std::uint32_t>(texture.mBytes[at + offset]) / 255.0f;
            };

            // The two loose spellings differ only in which end the three colours are stated from,
            // and a reader that took one order for both draws the sky with its red and blue swapped.
            if (texture.mFormat == TextureFormat::Bgra8Srgb)
                return osg::Vec3f(channel(2), channel(1), channel(0));

            return osg::Vec3f(channel(0), channel(1), channel(2));
        }

        // The colour half is the last eight bytes of a block whichever format it is: BC2 and BC3 put
        // their alpha in front of it and BC1 has none.
        const std::uint32_t columns = (level.mWidth + 3) / 4;
        const std::size_t at = level.mOffset + (std::size_t{ y / 4 } * columns + x / 4) * bytes + (bytes - 8);
        const ColourBlock block
            = ColourBlock::read(texture.mBytes.subspan(at).first<8>(), texture.mFormat == TextureFormat::Bc1RgbaSrgb);

        return block.mPalette[block.indexAt(std::size_t{ y % 4 } * 4 + x % 4)];
    }

    osg::Vec3f MeanTexel::opaque() const
    {
        if (!(mAlpha > 0.0f))
            return osg::Vec3f();

        return mColour / mAlpha;
    }

    MeanTexel meanTexel(const osg::Image& image)
    {
        std::vector<MipLevel> levels;

        TextureData described;
        try
        {
            described = describeImage(image, levels);
        }
        catch (const Error& what)
        {
            // A format nothing in the game produces, which is a mod's business rather than a broken
            // contract — the caller gets nothing and carries on without whatever this was worth.
            Log(Debug::Warning) << "cannot average \"" << image.getFileName() << "\": " << what.what();
            return MeanTexel();
        }

        if (levels.empty())
            return MeanTexel();

        // The finest level alone. A mip chain is the same picture at lower rates, so every level
        // holds the same mean to within its own filtering, and the coarse ones cost nothing to skip.
        const MipLevel& level = levels.front();
        if (level.mWidth == 0 || level.mHeight == 0)
            return MeanTexel();

        // Never empty here, because it is empty only for a description carrying no texels — so
        // the alpha is read rather than defaulted, which is the difference between a star sheet
        // worth nearly nothing and one worth the black it is painted on.
        const AlphaImage alpha(described);

        osg::Vec3d total;
        double covered = 0.0;
        for (std::uint32_t y = 0; y < level.mHeight; ++y)
            for (std::uint32_t x = 0; x < level.mWidth; ++x)
            {
                const osg::Vec3f stored = texelAt(described, level, x, y);
                const double opacity = alpha.at(0, x, y) / 255.0;

                total += osg::Vec3d(toLinear(stored)) * opacity;
                covered += opacity;
            }

        const double texels = double(level.mWidth) * level.mHeight;
        total /= texels;

        return MeanTexel{
            .mColour = osg::Vec3f(float(total.x()), float(total.y()), float(total.z())),
            .mAlpha = float(covered / texels),
        };
    }

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

    void writePng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> pixels)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(static_cast<int>(width), static_cast<int>(height), 1, GL_RGBA, GL_UNSIGNED_BYTE);

        const std::size_t stride = std::size_t{ width } * 4;
        for (std::uint32_t y = 0; y < height; ++y)
            std::memcpy(image->data(0, static_cast<int>(height - 1 - y)), pixels.data() + y * stride, stride);

        if (!osgDB::writeImageFile(*image, Files::pathToUnicodeString(path)))
            throw Error("cannot write " + Files::pathToUnicodeString(path));
    }

    PngImage readPng(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
            return PngImage{};

        const osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(Files::pathToUnicodeString(path));
        if (image == nullptr || image->s() <= 0 || image->t() <= 0 || image->getDataType() != GL_UNSIGNED_BYTE)
            return PngImage{};

        const int channels = image->getPixelFormat() == GL_RGBA ? 4 : image->getPixelFormat() == GL_RGB ? 3 : 0;
        if (channels == 0)
            return PngImage{};

        PngImage read{ static_cast<std::uint32_t>(image->s()), static_cast<std::uint32_t>(image->t()), {} };
        read.mPixels.resize(std::size_t{ read.mWidth } * read.mHeight * 4);

        // Back the way `writePng` sent them: OSG's first row is the bottom one.
        for (std::uint32_t y = 0; y < read.mHeight; ++y)
        {
            const unsigned char* row = image->data(0, static_cast<int>(read.mHeight - 1 - y));
            std::uint8_t* into = read.mPixels.data() + std::size_t{ y } * read.mWidth * 4;

            for (std::uint32_t x = 0; x < read.mWidth; ++x)
            {
                std::memcpy(into + std::size_t{ x } * 4, row + std::size_t{ x } * channels, channels);
                if (channels == 3)
                    into[std::size_t{ x } * 4 + 3] = 255;
            }
        }

        return read;
    }
}
