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

/// How much the curve takes off the darkest channel once it has any to take. Khronos's own.
RTX_CONST float TONE_SHADOW_OFFSET = 0.04;

/// Where the curve below stops leaving a colour alone and starts bringing it down.
///
/// Khronos's own, less the shadow offset, which it has already taken off by then.
RTX_CONST float TONE_COMPRESSION_START = 0.8 - TONE_SHADOW_OFFSET;

/// How far a compressed colour is carried toward white. Khronos's own.
RTX_CONST float TONE_DESATURATION = 0.15;

/// Radiance to a display range: Khronos PBR Neutral, with its shadow offset ramped.
///
/// **Chosen for what it does to a flame.** Every operator has to bring a highlight down, and this
/// one desaturates toward white as it compresses — so a torch goes white the way a photograph of one
/// does, rather than clipping channel by channel and passing through yellow and orange on its way.
/// Below `TONE_COMPRESSION_START` it is the identity, so a midtone is left exactly where the
/// exposure put it.
///
/// **The shadow offset is ramped rather than taken whole, and that is a departure from the
/// published curve.** Khronos takes a flat 0.04 off every channel, and for the darkest channel below
/// 0.08 takes `x - 6.25x^2` instead — which leaves that channel at exactly `6.25x^2`, a log-log
/// slope of two, doubling the contrast through the whole bottom of the range. A linear 0.01 keeps
/// six per cent of itself. And because the same amount comes off all three channels while only the
/// smallest is squared, a night colour arrives with its blue-to-red ratio several times inflated.
///
/// What the offset is for does not apply here: it cancels the four per cent Fresnel floor of a
/// dielectric so that a glTF `baseColor` reproduces exactly under even white light, which is a
/// colour-management guarantee for an asset viewer. Removing it outright is worse than keeping it,
/// because it does real shadow-contrast work by day. Ramping the amount away with the colour keeps
/// that and multiplies nothing by its own smallness: black stays black, and above three times the
/// offset this is the published curve bit for bit.
RTX_SHADER vec3 toneMap(vec3 colour)
{
    const float darkest = min(colour.x, min(colour.y, colour.z));
    colour -= TONE_SHADOW_OFFSET * clamp(darkest / (3.0 * TONE_SHADOW_OFFSET), 0.0, 1.0);

    const float peak = max(colour.x, max(colour.y, colour.z));
    if (peak < TONE_COMPRESSION_START)
        return colour;

    // A hyperbola through the compression point, asymptotic to one: the whole range above it is
    // brought inside the display's without ever reaching the end of it.
    const float span = 1.0 - TONE_COMPRESSION_START;
    const float brought = 1.0 - span * span / (peak + span - TONE_COMPRESSION_START);
    colour *= brought / peak;

    const float toward = 1.0 - 1.0 / (TONE_DESATURATION * (peak - brought) + 1.0);
    return mix(colour, vec3(brought), toward);
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
