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
    /// A rectangle of an image, in texels from its corner.
    ///
    /// **Named rather than four loose integers**, because the two functions below take two
    /// rectangles apiece and a transposed pair is a picture that is wrong in a way nothing asserts.
    struct Rect
    {
        int mX = 0;
        int mY = 0;
        int mWidth = 0;
        int mHeight = 0;
    };

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

    /// One texel of `image` at `u`, `v`, filtered the way a sampler set to `GL_LINEAR` and
    /// `GL_CLAMP_TO_EDGE` filters one, into `out` as four bytes.
    ///
    /// **A device's own answer, worked out here.** The world map overlay used to be composited by a
    /// camera drawing a local map tile into it, so what a cell looks like on the map is what that
    /// sampler made of it: four texels of a fourteen-fold reduction, aliasing and all. The
    /// compositing moved to main memory and the picture has to stay where it was — an average over
    /// the whole footprint is a better filter and a different map, which is a change to what the
    /// game shows rather than to how it is drawn.
    ///
    /// The image must be four bytes a pixel.
    void sampleBilinear(const osg::Image& image, float u, float v, std::uint8_t (&out)[4]);

    /// Scales `source` of `from` into `target` of `into`, filtered exactly as `sampleBilinear`
    /// filters and clamped to `source` rather than to the image around it.
    ///
    /// **One filter and not a second one.** A savegame written at another map resolution lands on a
    /// grid that lines up with nothing, and what fills it has to be the sampler the rest of the map
    /// was drawn through — a nearer or a wider filter there is a seam between the tiles a player
    /// walked before the resolution changed and the ones they walked after.
    ///
    /// Both images must be four bytes a pixel, and both rectangles must lie inside their own.
    void resampleRegion(const osg::Image& from, const Rect& source, osg::Image& into, const Rect& target);

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
    void gatherRegion(const osg::Image& image, const Rect& area, std::vector<std::uint8_t>& rows);
}

#endif
