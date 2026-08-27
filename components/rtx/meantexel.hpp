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
    /// share. A gather wants exactly that: its lobe is a hemisphere where a ray is a direction, so
    /// what a sheet contributes to it is the sheet's own average and not whatever one ray happened
    /// to point at.
    ///
    /// **Every texel and not a sample of them**, for the same reason. A thumbnail may read one texel
    /// in a few hundred and be right; a mean of a sheet that is mostly empty cannot.
    ///
    /// Nothing at all where the image is in a format `describeImage` does not read, which is a
    /// sheet this cannot answer for rather than one worth nothing.
    osg::Vec3f meanTexel(const osg::Image& image);
}
