// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_RANDOM_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_RANDOM_GLSL

// Blue noise across the screen, a low-discrepancy sequence along time, and what a pair of those
// numbers becomes when a shadow ray or a bounce asks for a direction.

#include "scene.h"
#include "bindings.glsl"

/// Which sequence a lamp reservoir draws on. **One per depth, because a path shades twice** — the
/// hit the eye found and the hit its bounce found — and two reservoirs stepping the same sequence
/// would choose correlated lamps at both ends of it. These are seeds for `randomSeed` rather than
/// channels of the tile, which has only `RANDOM_STREAMS` of them and answers a different question.
///
/// **Each one is the one before it and one more, and none of them is a number written out.** What a
/// seed owes the others is only that it differ from them, and a list of literals states that in a
/// way nothing checks — two of them spelled alike compile, and the two reservoirs then draw one
/// sequence and keep the same lamps.
const uint SEED_LAMPS_EYE = 0x51u;
const uint SEED_LAMPS_BOUNCE = SEED_LAMPS_EYE + 1u;

/// And a third for the pane the eye is looking through, which shades beside the surface behind it
/// and would choose the same lamp at both if it stepped the same sequence.
const uint SEED_LAMPS_PANE = SEED_LAMPS_BOUNCE + 1u;

/// Water shades two surfaces from one hit — what it reflects and what is seen through it — and each
/// of them opens a reservoir of its own. **Two constants and not one**, because two reservoirs
/// seeded alike step the same sequence and keep the same lamps.
const uint SEED_LAMPS_MIRROR = SEED_LAMPS_PANE + 1u;
const uint SEED_LAMPS_THROUGH = SEED_LAMPS_MIRROR + 1u;

/// And one more for the direction a path's end asks the sky about, which is not a lamp at all.
///
/// **A sequence of its own, because it is drawn beside a reservoir and not out of one.** Stepping
/// the lamps' would move which lamp a hit chose every time the hemisphere was asked a question, and
/// the two have nothing to do with each other.
const uint SEED_SKY_REACHING = SEED_LAMPS_THROUGH + 1u;

/// And one for where on the sun's disc a sprite layer's shadow ray leaves from.
///
/// **Not the one above, which is drawn in the same breath.** Two draws seeded alike take the same
/// numbers, so the point on the disc and the direction into the sky would move together across the
/// whole frame — a pattern rather than noise, and the filter keeps a pattern.
const uint SEED_SPRITE_SUN = SEED_SKY_REACHING + 1u;

/// And one for the lamp a fog march holds out of every lamp at every one of its steps.
const uint SEED_LAMPS_FOG = SEED_SPRITE_SUN + 1u;

/// And one for the lamp a layer of sprites holds, beside the sun's and the sky's own rays there.
const uint SEED_LAMPS_SPRITE = SEED_LAMPS_FOG + 1u;

/// And one for which face of a sheet the eye's bounce leaves by.
///
/// **Beside the bounce's own pair and not out of it.** The direction is drawn from the tile's
/// `STREAM_BOUNCE`, which has two channels and no third; a side taken from one of them would tie
/// which face is asked to where in the hemisphere it is asked, and a sheet's two faces would be
/// sampled as two halves of one hemisphere rather than as two hemispheres.
const uint SEED_SHEET_SIDE = SEED_LAMPS_SPRITE + 1u;

/// How far each stream's sequence advances between frames.
///
/// **An additive recurrence with an irrational step**, which is the cheapest sequence whose every
/// prefix covers `[0, 1)` evenly rather than only its powers of two. The fog draws one number and
/// takes the golden ratio; the bounce draws a pair and takes R2's steps, the plastic constant's
/// first two powers, which is the same construction in two dimensions.
///
/// A rational step would close into a cycle and the frames after it would resample what the ones
/// before had already asked.
///
/// **The one part of the stream table that stays here**, because a constant array is spelled
/// `float[](...)` in GLSL and `{...}` in C++ and there is no third spelling both compile. It is
/// `RANDOM_STREAMS` long by declaration, so the count still binds it; what a second shader needs to
/// know — which channels are taken — is `STREAM_FOG`, `STREAM_BOUNCE` and `STREAM_WATER`, and those
/// sit with the count in `scene.h`.
///
/// The golden ratio for one number, the two-dimensional `R2` pair for the bounce, and `sqrt(2) - 1`
/// for the water — a fourth irrational rather than a second copy of the first, because two streams
/// turning by the same step differ only by where they started and converge on the same sweep.
const float STREAM_TURN[RANDOM_STREAMS] = float[](0.6180340, 0.7548777, 0.5698403, 0.4142136);

/// One number in `[0, 1)` for `pixel`, from this frame's `stream`th draw.
///
/// **Blue noise across the screen, a low-discrepancy sequence along time.** The tile decides how a
/// pixel's draw differs from its neighbours' — deliberately, so that the error between them
/// alternates rather than clumping into blotches a filter would read as shading. The turn decides
/// how it differs from its own last frame, so the samples a pixel accumulates sweep the interval
/// instead of stumbling about in it.
///
/// Shifting every value by the same amount and wrapping is Cranley and Patterson's rotation: it
/// moves which pixel holds which number and leaves the arrangement's spectrum where it was.
float randomAt(uvec2 pixel, uint stream)
{
    const uvec2 tile = pixel % BLUE_NOISE_EXTENT;
    const uint at = (tile.y * BLUE_NOISE_EXTENT + tile.x) * RANDOM_STREAMS + stream;

    return fract(blueNoise[at] + float(frame.mFrame) * STREAM_TURN[stream]);
}

/// Two numbers in `[0, 1)` for one pixel, from `stream` and the one after it.
vec2 unitPair(uvec2 pixel, uint stream)
{
    return vec2(randomAt(pixel, stream), randomAt(pixel, stream + 1u));
}

/// A stream of draws for one pixel, where the tile above gives one.
///
/// **The tile answers a different question and cannot be stretched to this one.** Blue noise is an
/// arrangement *across the screen*: it says how a pixel's draw should differ from its neighbours',
/// which is what makes a single sample per pixel filter well. Resampling needs a *sequence* — a
/// fresh number for each candidate it weighs — and there is no screen-space arrangement of a
/// sequence to arrange. Asking the tile for one would hand back the same number every time and
/// choose the first candidate that beat it, every pixel, every frame.
///
/// So this is an ordinary hashed counter, seeded per pixel and per frame. PCG's output permutation
/// over an LCG state: the state advances by multiplication and the bits are mixed on the way out,
/// which is what keeps low-order structure out of the first few draws — the ones a short reservoir
/// loop actually uses.
uint randomSeed(uint key)
{
    // The frame is mixed in here rather than by the caller, so a sequence advances between frames
    // without anyone having to remember to make it — which is what lets the accumulator in front of
    // the filter see an independent draw each time rather than the same one over and over.
    uint state = key * 0x9E3779B9u + frame.mFrame * 0xC2B2AE35u;
    state ^= state >> 16u;
    state *= 0x7FEB352Du;

    return state;
}

/// A key for one pixel, which a caller offsets by a `SEED_` constant to say which sequence it wants.
///
/// Two odd multipliers rather than two shifts: a shift leaves the low bits of one axis where the
/// other's are, and two pixels a power of two apart then share a prefix.
uint pixelKey(uvec2 pixel)
{
    return pixel.x * 73856093u ^ pixel.y * 19349663u;
}

float randomNext(inout uint state)
{
    state = state * 747796405u + 2891336453u;

    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word ^= word >> 22u;

    // Twenty-four bits, which is every one a float can hold without rounding two of them together.
    return float(word >> 8u) * (1.0 / 16777216.0);
}

/// A unit vector square to `axis`, to build a basis on.
///
/// Any vector not parallel to it will do, and which one is arbitrary — so the only thing this owes
/// a caller is that the cross product it takes never collapses.
vec3 tangentTo(vec3 axis)
{
    const vec3 aside = abs(axis.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(aside, axis));
}

/// A direction inside the cone about `axis` that a source subtends, drawn evenly over its solid
/// angle.
///
/// **This is the whole of what a soft shadow is.** A source with a size is not one direction but a
/// cone of them, and a shadow ray drawn from somewhere in that cone rather than down its axis puts
/// a penumbra under every occluder whose width is the source's own size seen from it. Evenly over
/// the solid angle is evenly in the cosine, which is the right draw for a disc of uniform radiance
/// and leaves nothing to weigh the sample by.
///
/// @param sine the sine of the cone's half-angle: the source's radius over its distance, and for
///        the sun a constant. Zero is a point source and returns `axis` exactly, which is what
///        keeps a light with no size casting the edge it used to.
vec3 coneDirection(vec3 axis, float sine, vec2 u)
{
    const float cosine = sqrt(max(1.0 - sine * sine, 0.0));

    // `1 - cos(half-angle)`, written so that it is never a subtraction of two numbers that are
    // nearly equal. The sun is half a degree across and its cosine is 0.99999, so taking that from
    // one spends five of a float's seven digits before the draw has begun — and every one of them
    // is a step of the penumbra it is about to place.
    const float versine = sine * sine / (1.0 + cosine);

    const float drop = u.x * versine;
    const float radius = sqrt(drop * (2.0 - drop));
    const float turn = TAU * u.y;

    const vec3 tangent = tangentTo(axis);
    return tangent * (radius * cos(turn)) + cross(axis, tangent) * (radius * sin(turn)) + axis * (1.0 - drop);
}

/// A direction about `normal`, drawn with probability proportional to its cosine.
///
/// **The one distribution that cancels the cosine term.** A diffuse surface weights what arrives by
/// `cos / pi` and this draws in exactly that proportion, so the estimator is the incoming radiance
/// itself with no weight left to carry — which is why a single sample is worth anything at all.
///
/// Malley's method: a disc sampled evenly, lifted onto the hemisphere. `sqrt(u.x)` is the disc's
/// radius, so the height off the surface is `sqrt(1 - u.x)` and averages two thirds — which is the
/// number a test can hold this to, and the half a uniform draw would give instead.
vec3 cosineDirection(vec3 normal, vec2 u)
{
    const float radius = sqrt(u.x);
    const float angle = TAU * u.y;

    const vec3 tangent = tangentTo(normal);

    return tangent * (radius * cos(angle)) + cross(normal, tangent) * (radius * sin(angle))
        + normal * sqrt(max(1.0 - u.x, 0.0));
}

#endif
