#pragma once

#include <osg/Vec3f>

namespace osg
{
    class Image;
}

namespace Rtx
{
    /// What one texel of an image is worth on average, linear and premultiplied by its own alpha.
    ///
    /// **What a sheet is worth as a light, in one number.** A sky sheet is drawn as `rgb * a` and
    /// covers a known piece of sky, so what it adds to the sky's mean radiance is this times that
    /// share — and a mean is the only form of it a gather can afford. Sampling `tx_stars` per ray
    /// instead would be a firefly at every hit: 0.8% of its texels carry nearly all of its light,
    /// and the sheets ship no mip chain to blur it away with.
    ///
    /// **Every texel and not a sample of them**, for the same reason. A thumbnail may read one texel
    /// in a few hundred and be right; a mean of a sheet that is mostly empty cannot.
    ///
    /// Nothing at all where the image is in a format `describeImage` does not read, which is a
    /// sheet this cannot answer for rather than one worth nothing.
    osg::Vec3f meanTexel(const osg::Image& image);
}
