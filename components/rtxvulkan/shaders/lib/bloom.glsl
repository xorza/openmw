// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BLOOM_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BLOOM_GLSL

// The two kernels the pyramid is built and taken apart with.
//
// **One copy, because three passes ask for them**: `bloomdown.comp` halving the frame and then each
// level, `bloomup.comp` spreading each level into the one above it, and `tone.comp` spreading the
// finest one over the picture. The last two run the same nine taps, and the display pass runs them
// because that is where the veil belongs — `BloomPass` says why nothing writes the frame.
//
// Both are Jorge Jimenez's, out of *Next Generation Post Processing in Call of Duty: Advanced
// Warfare* (SIGGRAPH 2014), and both lean on the sampler: every tap sits on a texel corner, so the
// hardware's bilinear fetch reads four texels for each one written here — thirteen taps for
// thirty-six texels going down, nine for thirty-six coming back up.

/// The frame or a level, halved.
///
/// **Thirteen taps in five overlapping squares rather than one box.** A plain 2x2 box halved twice
/// is a wider box, and a wider box has a flat top and a sharp edge — which is what makes a cheap
/// bloom shimmer as a highlight crosses a texel boundary of a level nobody is looking at. The
/// overlap is what smooths the transfer between levels, and the corner weights are what taper it.
///
/// @param uv where the destination texel's centre lands in the source, which for an exact halving
///        is the same point in both.
/// @param texel one source texel in source texture coordinates.
vec3 bloomHalved(sampler2D source, vec2 uv, vec2 texel)
{
    const vec2 wide = 2.0 * texel;

    const vec3 a = texture(source, uv + vec2(-wide.x, wide.y)).rgb;
    const vec3 b = texture(source, uv + vec2(0.0, wide.y)).rgb;
    const vec3 c = texture(source, uv + wide).rgb;

    const vec3 d = texture(source, uv + vec2(-wide.x, 0.0)).rgb;
    const vec3 e = texture(source, uv).rgb;
    const vec3 f = texture(source, uv + vec2(wide.x, 0.0)).rgb;

    const vec3 g = texture(source, uv - wide).rgb;
    const vec3 h = texture(source, uv + vec2(0.0, -wide.y)).rgb;
    const vec3 i = texture(source, uv + vec2(wide.x, -wide.y)).rgb;

    const vec3 j = texture(source, uv + vec2(-texel.x, texel.y)).rgb;
    const vec3 k = texture(source, uv + texel).rgb;
    const vec3 l = texture(source, uv - texel).rgb;
    const vec3 m = texture(source, uv + vec2(texel.x, -texel.y)).rgb;

    // A partition of one: an eighth on the centre, an eighth spread over the four corners, a
    // quarter over the four edges, and a half over the inner square.
    return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}

/// A level, spread over the one above it.
///
/// A 3x3 tent, which is the widest kernel a doubling can carry without reaching past the texels the
/// level below actually holds.
///
/// @param texel one source texel, so the tent is a fixed shape in the coarser grid and twice the
///        span in the finer one.
vec3 bloomSpread(sampler2D source, vec2 uv, vec2 texel)
{
    const vec3 a = texture(source, uv + vec2(-texel.x, texel.y)).rgb;
    const vec3 b = texture(source, uv + vec2(0.0, texel.y)).rgb;
    const vec3 c = texture(source, uv + texel).rgb;

    const vec3 d = texture(source, uv + vec2(-texel.x, 0.0)).rgb;
    const vec3 e = texture(source, uv).rgb;
    const vec3 f = texture(source, uv + vec2(texel.x, 0.0)).rgb;

    const vec3 g = texture(source, uv - texel).rgb;
    const vec3 h = texture(source, uv + vec2(0.0, -texel.y)).rgb;
    const vec3 i = texture(source, uv + vec2(texel.x, -texel.y)).rgb;

    return (e * 4.0 + (b + d + f + h) * 2.0 + (a + c + g + i)) * (1.0 / 16.0);
}

#endif
