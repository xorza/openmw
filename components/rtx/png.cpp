#include "png.hpp"

#include <cstring>

#include <osg/Image>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>

#include <components/files/conversion.hpp>

#include "error.hpp"

namespace Rtx
{
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
