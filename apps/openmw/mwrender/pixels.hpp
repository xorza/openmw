#ifndef GAME_RENDER_PIXELS_H
#define GAME_RENDER_PIXELS_H

#include <cstdint>

namespace osg
{
    class Image;
}

namespace MWRender
{
    /// A rectangle of an image, in texels from its corner. Named because the functions below take
    /// two apiece, and a transposed pair is a picture that is wrong in a way nothing asserts.
    struct Rect
    {
        int mX = 0;
        int mY = 0;
        int mWidth = 0;
        int mHeight = 0;
    };

    /// One texel of `image` at `u`, `v`, filtered the way a sampler set to `GL_LINEAR` and
    /// `GL_CLAMP_TO_EDGE` filters one, into `out` as four bytes. The world map is composited in
    /// main memory and has to look as it did when a camera composited it: four texels of a
    /// fourteen-fold reduction, aliasing and all. The image must be four bytes a pixel.
    void sampleBilinear(const osg::Image& image, float u, float v, std::uint8_t (&out)[4]);

    /// Scales `source` of `from` into `target` of `into`, filtered exactly as `sampleBilinear`
    /// filters and clamped to `source`. A savegame written at another map resolution is refilled
    /// through the sampler the rest of the map was drawn through, or the tiles walked before the
    /// change and after it meet at a seam. Both images four bytes a pixel, both rectangles inside
    /// their own image.
    void resampleRegion(const osg::Image& from, const Rect& source, osg::Image& into, const Rect& target);
}

#endif
