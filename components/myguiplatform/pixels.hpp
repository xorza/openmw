#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_PIXELS_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_PIXELS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <MyGUI_RenderFormat.h>

namespace osg
{
    class Image;
}

namespace MyGUIPlatform
{
    /// What MyGUI can be handed a row at a time, or nothing where the image has to be read pixel by
    /// pixel first. The common cases — a decoded frame, a screenshot, a map — are all in here.
    std::optional<MyGUI::PixelFormat> directFormat(const osg::Image& image);

    /// How many bytes one pixel of `format` occupies.
    std::size_t bytesPerPixel(MyGUI::PixelFormat format);

    /// Writes `image` into `into` as four bytes a pixel, row zero first — `image.s() * image.t()`
    /// pixels of it, and the caller owns the room for them.
    ///
    /// **One `memcpy` where the image already is that, and a pixel at a time where it is not.**
    /// OpenSceneGraph hands back whatever the file held — three channels, one channel, a row pitch
    /// of its own — and `osg::Image::getColor` is the only thing that reads all of them. It is also
    /// a virtual call and a `Vec4f` per pixel, which is worth not paying for the case that is most
    /// of them.
    ///
    /// **Neutral, despite where it lives**, for the same reason `Picture` is: nothing here says
    /// what draws. Both backends widen images for MyGUI, and only one of them used to do it the
    /// quick way.
    void writeRgba(const osg::Image& image, std::uint8_t* into);

    /// Copies a rectangle of `image` into `rows`, tightly packed, four bytes a pixel, row zero
    /// first — `height` rows of `width` pixels and nothing between them.
    ///
    /// **What a backend that can take a rectangle has to be handed.** The image's own rows are as
    /// wide as the image, so a region inside one is not a run of bytes; this is where it becomes
    /// one. `rows` is resized and refilled, so a caller writing part of a picture again and again
    /// allocates once.
    ///
    /// The image must be four bytes a pixel with contiguous data and the rectangle must lie inside
    /// it, which are contracts on the caller.
    void gatherRegion(const osg::Image& image, int x, int y, int width, int height, std::vector<std::uint8_t>& rows);
}

#endif
