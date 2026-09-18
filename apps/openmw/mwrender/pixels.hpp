#ifndef GAME_RENDER_PIXELS_H
#define GAME_RENDER_PIXELS_H

#include <cstdint>

#include <osg/ref_ptr>

#include <components/sceneutil/imageregion.hpp>

namespace osg
{
    class Image;
}

namespace MWRender
{
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
    void resampleRegion(const osg::Image& from, const SceneUtil::ImageRegion& source, osg::Image& into,
        const SceneUtil::ImageRegion& target);

    /// `image` as the tightly packed RGBA bytes a painted picture is: the image itself where it
    /// already is that, and a converted copy otherwise — what a png or tga reader hands back for a
    /// saved map is its own affair.
    osg::ref_ptr<osg::Image> asRgba(osg::ref_ptr<osg::Image> image);
}

#endif
