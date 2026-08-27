#pragma once

#include <osg/Vec3f>

namespace osg
{
    class Image;
}

namespace Rtx
{
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
}
