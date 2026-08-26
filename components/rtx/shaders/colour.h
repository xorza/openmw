// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_COLOUR_H
#define OPENMW_COMPONENTS_RTX_SHADERS_COLOUR_H

#include "portable.h"

#ifdef RTX_HOST

#include <osg/Vec3f>

namespace Rtx::Shaders
{
    using vec3 = osg::Vec3f;

#endif

    /// How this renderer weighs a colour into a brightness.
    ///
    /// Rec. 709, which is what these primaries are.
    ///
    /// **Shared because two shaders now decide something by it**, and a pair of weights that
    /// disagreed would be two different ideas of which of two things is brighter. The exposure
    /// histogram measures the frame with it; the trace asks whether the sprites over a pixel put
    /// more light into it than the surface behind them left.
    RTX_CONST vec3 LUMINANCE_WEIGHTS = vec3(0.2126f, 0.7152f, 0.0722f);

#ifdef RTX_HOST
}
#endif

// What both shading languages read and the host does not, for the reason `RTX_SHADER` gives.
#ifndef RTX_HOST

/// The largest of a colour's three channels.
///
/// **Not the luminance above, and the difference is what each is for.** A luminance asks how bright
/// something looks and weighs the channels by the eye; this asks how much of a colour there is at
/// all, which is the question a threshold wants — whether the sun puts more into the air than the
/// sky does, and how bright to draw a disc whose hue comes from somewhere else.
RTX_SHADER float brightest(vec3 colour)
{
    return max(colour.x, max(colour.y, colour.z));
}

/// The sRGB transfer curve, linear radiance to what a display expects of a byte.
///
/// The piecewise form and not the 2.2 approximation. The two differ by several per cent in the
/// darks, which is where a bounce puts most of what it has to say.
///
/// **Per component, because the two shading languages spell a vector select differently** — GLSL
/// picks a side with `mix` over a `bvec3`, Metal with `select`, and a ternary is the one form both
/// read. Nothing changes by it: a select with a boolean weight picks a side rather than blending
/// toward one.
RTX_SHADER float encodeSrgb(float linear)
{
    if (linear <= 0.0031308)
        return linear * 12.92;

    return 1.055 * pow(max(linear, 0.0), 1.0 / 2.4) - 0.055;
}

RTX_SHADER vec3 encodeSrgb(vec3 linear)
{
    return clamp(vec3(encodeSrgb(linear.x), encodeSrgb(linear.y), encodeSrgb(linear.z)), vec3(0.0), vec3(1.0));
}

#endif

#endif
