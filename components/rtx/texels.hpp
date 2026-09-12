#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <osg/Vec3f>

#include "texturedata.hpp"

namespace osg
{
    class Image;
}

namespace Rtx
{
    /// The colour of one texel of one level, as it is stored — display-encoded; `Rtx::toLinear`
    /// turns it into light. A texel at a time rather than a level decoded first, because both
    /// callers ask for a scattered few. `x` and `y` must lie inside `level`.
    osg::Vec3f texelAt(const TextureData& texture, const MipLevel& level, std::uint32_t x, std::uint32_t y);

    /// What one texel of an image is worth on average.
    struct MeanTexel
    {
        /// The mean colour, linear and premultiplied by its own alpha — what a sheet is worth as a
        /// light, since it is drawn as `rgb * a` over a known piece of sky.
        osg::Vec3f mColour;

        /// The mean of its own alpha — what tells a wisp from a lid, since a few bright clouds over
        /// an empty sky average to the same colour as a solid grey one.
        float mAlpha = 0.0f;

        /// The mean of what that alpha calls solid: `mColour` with the cover divided back out, which
        /// is what a texel is read as a ratio to where the painting is used for its shape. Nothing
        /// where nothing is painted.
        osg::Vec3f opaque() const;
    };

    /// Averages `image`, every texel and not a sample, because a mean of a sheet that is mostly
    /// empty cannot be sampled. Nothing where the image is in a format `describeImage` does not
    /// read.
    MeanTexel meanTexel(const osg::Image& image);

    /// What OpenSceneGraph decoded a texture into — the content's own format, where
    /// `TextureFormat` holds only the ones this renderer can take, because a file the uploader
    /// refuses is still a file the report has to name.
    enum class ImageFormat : std::uint8_t
    {
        Bc1,
        Bc2,
        Bc3,
        Rgb8,
        Rgba8,
        Bgra8,
        Luminance,
        LuminanceAlpha,

        /// Anything else, and there is one count of them rather than one each.
        Unnamed,
    };

    constexpr std::size_t sImageFormatCount = static_cast<std::size_t>(ImageFormat::Unnamed) + 1;

    /// Which format `image` arrived in — the one place a `GLenum` decides anything, so the
    /// uploader and the report cannot disagree.
    ImageFormat readFormat(const osg::Image& image);

    /// What `format` is called, for a report to print.
    std::string_view nameOf(ImageFormat format);

    /// Writes tightly packed 8-bit RGBA, top row first, as a PNG. The renderer writes row zero at
    /// the top and OSG's images start at the bottom, so this flips on the way through. Throws when
    /// the file cannot be written.
    void writePng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> pixels);

    /// A picture in the layout `writePng` takes: tightly packed 8-bit RGBA, top row first.
    struct PngImage
    {
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::vector<std::uint8_t> mPixels;

        bool empty() const { return mPixels.empty(); }
    };

    /// Reads a PNG back into that layout. Empty where the file is missing or is not eight-bit
    /// colour, which a caller comparing two runs reports rather than throws over.
    PngImage readPng(const std::filesystem::path& path);
}
