#pragma once

#include <cstdint>
#include <span>

#include <osg/Image>
#include <osg/ref_ptr>

namespace Rtx
{
    /// A traced frame as a backend hands it over: tightly packed 8-bit RGBA, row zero at the top.
    struct TracedFrame
    {
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::span<const std::uint8_t> mPixels;
    };

    /// Which end of the picture row zero of the result holds.
    enum class RowOrder
    {
        /// The trace's own, and what MyGUI takes: `MyGUIPlatform::Picture` copies an image straight
        /// into a locked texture and the interface draws it from the top down.
        TopFirst,

        /// OpenSceneGraph's, and what `osgDB`'s writers and a savegame thumbnail expect.
        BottomFirst,
    };

    /// How many bytes a pixel of the result carries, which is its pixel format.
    enum class Channels : int
    {
        /// The frame's own, and what a locked texture and a PNG take.
        Rgba = 4,

        /// A savegame thumbnail's: its writer is JPEG, which has no alpha to carry and refuses a
        /// four-channel image outright — an `ERROR_IN_WRITING_FILE` and a save with no picture in
        /// it, which is what the rasterizer avoids by reading its screenshots back as `GL_RGB`.
        Rgb = 3,
    };

    /// The frame as an `osg::Image` of the size and the format asked for, or null where there is
    /// nothing to give.
    ///
    /// **Nearest, and resampled here rather than by `osg::Image::scaleImage`**, which is
    /// `gluScaleImage` — a GL call, and on this path there is no context to make it in. A save asks
    /// for its thumbnail at a hundred pixels across, where a box filter rounds to the same texels.
    ///
    /// @return null where either extent is zero or `frame.mPixels` is shorter than the frame it
    ///         claims to be, because a picture of part of a frame is worse than none.
    osg::ref_ptr<osg::Image> frameImage(
        const TracedFrame& frame, int width, int height, RowOrder order, Channels channels = Channels::Rgba);
}
