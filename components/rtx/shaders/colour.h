#ifndef OPENMW_COMPONENTS_RTX_SHADERS_COLOUR_H
#define OPENMW_COMPONENTS_RTX_SHADERS_COLOUR_H

#include "hosttypes.h"
#include "look.h"
#include "portable.h"

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// How this renderer weighs a colour into a brightness.
    ///
    /// Rec. 709, which is what these primaries are.
    ///
    /// **Shared because two shaders now decide something by it**, and a pair of weights that
    /// disagreed would be two different ideas of which of two things is brighter. The exposure
    /// histogram measures the frame with it; the trace asks whether the sprites over a pixel put
    /// more light into it than the surface behind them left.
    const vec3 LUMINANCE_WEIGHTS = vec3(0.2126f, 0.7152f, 0.0722f);

    /// One channel of a colour carried toward its luminance: nought gives the luminance, one the
    /// channel, and past one the channel further from it — `TONE_SATURATION`'s grade.
    ///
    /// **Two products, so that one gives the channel back to the bit** and a saturation of one
    /// changes no picture. The usual `luminance + saturation * (channel - luminance)` rounds twice
    /// there. Held at nought past one, where a channel under the luminance is carried below nothing.
    RTX_SHADER float saturatedChannel(float channel, float luminance, float saturation)
    {
        const float carried = channel * saturation + luminance * (1.0f - saturation);
        return saturation > 1.0f ? max(carried, 0.0f) : carried;
    }

    /// What a colour of `luminance` is multiplied by to put it `contrast` times as many stops from
    /// `EXPOSURE_KEY` — `TONE_CONTRAST`'s grade.
    ///
    /// **A contrast of one is one without the power**, which a device evaluates to a bound and not
    /// exactly, so a contrast of one changes no picture. Nothing is moved under the smallest normal
    /// float either, which a device may flush to nought — and the logarithm of nought is an infinity
    /// the power turns into a NaN.
    RTX_SHADER float contrastScale(float luminance, float contrast)
    {
        const float smallestNormal = 1.17549435e-38f;
        if (contrast == 1.0f || !(luminance >= smallestNormal))
            return 1.0f;

        return exp2((contrast - 1.0f) * log2(luminance / EXPOSURE_KEY));
    }

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

/// A colour spread about mid grey by `contrast` and carried toward its luminance by `saturation`:
/// `contrastScale`, then `saturatedChannel` in each channel.
///
/// One luminance for both, because the spread scales every channel alike and so scales the
/// luminance by the same number.
RTX_SHADER vec3 graded(vec3 colour, float contrast, float saturation)
{
    const float luminance = dot(colour, LUMINANCE_WEIGHTS);
    const float scale = contrastScale(luminance, contrast);
    const vec3 spread = colour * scale;
    const float spreadLuminance = luminance * scale;

    return vec3(saturatedChannel(spread.x, spreadLuminance, saturation),
        saturatedChannel(spread.y, spreadLuminance, saturation),
        saturatedChannel(spread.z, spreadLuminance, saturation));
}

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
/// **Per component, and a ternary rather than a vector select.** GLSL picks a side with `mix` over
/// a `bvec3`, which the host has no spelling for, so the one form both read is this. Nothing
/// changes by it: a select with a boolean weight picks a side rather than blending toward one.
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

/// The curve the other way: a stored value back to the linear radiance it stands for, which is
/// what a sampler does to a display-encoded texel and what a dispatch reading the bytes through a
/// `UNORM` view has to do itself. `Rtx::toLinear` is the host's spelling.
RTX_SHADER float decodeSrgb(float encoded)
{
    if (encoded <= 0.04045)
        return encoded / 12.92;

    return pow((encoded + 0.055) / 1.055, 2.4);
}

RTX_SHADER vec3 decodeSrgb(vec3 encoded)
{
    return vec3(decodeSrgb(encoded.x), decodeSrgb(encoded.y), decodeSrgb(encoded.z));
}

#endif

#endif
