#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace osg
{
    class Image;
}

namespace Rtx
{
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
}
