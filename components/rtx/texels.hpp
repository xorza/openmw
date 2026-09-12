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
    /// The colour of one texel of one level, as it is stored.
    ///
    /// Display-encoded for every content format, because that is what the file holds and what the
    /// sampler would have converted on the way in — `Rtx::toLinear` is what turns it into light.
    ///
    /// **A texel at a time rather than a level decoded first.** Both callers ask for a scattered
    /// few: a thumbnail reads one texel in a few hundred, and a composite reads one per output texel
    /// out of a ground texture it is minifying hard. Decoding the level would be most of the work
    /// for none of the answer.
    ///
    /// `x` and `y` must lie inside `level`, and `level` must be one of the texture's own.
    osg::Vec3f texelAt(const TextureData& texture, const MipLevel& level, std::uint32_t x, std::uint32_t y);

    /// What one texel of an image is worth on average.
    struct MeanTexel
    {
        /// The mean colour, linear and premultiplied by its own alpha.
        ///
        /// **What a sheet is worth as a light, in one number.** A sky sheet is drawn as `rgb * a`
        /// and covers a known piece of sky, so what it adds to the sky's mean radiance is this times
        /// that share. A gather wants exactly that: its lobe is a hemisphere where a ray is a
        /// direction, so what a sheet contributes to it is the sheet's own average and not whatever
        /// one ray happened to point at.
        osg::Vec3f mColour;

        /// The mean of its own alpha — how much of the image is there at all.
        ///
        /// **What tells a wisp from a lid, which `mColour` alone cannot.** A quarter for clear
        /// weather's cirrus sheet and all of it for every overcast one, and a few bright clouds over
        /// an empty sky average to the same colour as a solid grey one.
        float mAlpha = 0.0f;

        /// The mean of what that alpha calls solid: `mColour` with the cover divided back out.
        ///
        /// **A sheet's own paint, rather than what it adds to what is behind it.** It is what a
        /// texel is read as a ratio to where the painting is being used for its shape — a cirrus
        /// sheet is a quarter covered, and its clouds are not a quarter as bright as they look.
        ///
        /// Nothing where nothing is painted, which is a sheet that draws nothing anyway.
        osg::Vec3f opaque() const;
    };

    /// Averages `image`.
    ///
    /// **Every texel and not a sample of them.** A thumbnail may read one texel in a few hundred and
    /// be right; a mean of a sheet that is mostly empty cannot.
    ///
    /// Nothing at all where the image is in a format `describeImage` does not read, which is a
    /// sheet this cannot answer for rather than one worth nothing.
    MeanTexel meanTexel(const osg::Image& image);

    /// What OpenSceneGraph decoded a texture into.
    ///
    /// **The content's own format, and not what a backend uploads it as** — `TextureFormat` is
    /// that, and it holds only the ones this renderer can take. The two are one step apart and the
    /// step is `toTextureFormat`: a file the uploader refuses is still a file the report has to be
    /// able to name, which is the whole reason the report exists.
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

    /// Which format `image` arrived in.
    ///
    /// **The one place a `GLenum` decides anything.** What the uploader will take and what a report
    /// names are two questions with one answer between them, and asking OpenSceneGraph twice is how
    /// they come to disagree about a format one of them has met and the other has not heard of.
    ImageFormat readFormat(const osg::Image& image);

    /// What `format` is called, for a report to print.
    std::string_view nameOf(ImageFormat format);

    /// Writes tightly packed 8-bit RGBA, top row first, as a PNG.
    ///
    /// The renderer writes row zero at the top and OSG's images start at the bottom, so this flips
    /// on the way through. Throws when the file cannot be written.
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
