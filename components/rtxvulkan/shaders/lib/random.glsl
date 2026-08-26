// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_RANDOM_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_RANDOM_GLSL

// Blue noise across the screen, a low-discrepancy sequence along time, and a hash for the
// one field that asks about a place in the world rather than a pixel on the screen.

#include "scene.h"
#include "bindings.glsl"

/// Three integers to one number in `[0, 1]`: a repeatable value for a cell of the noise field.
///
/// **The field's lattice and nothing else.** It fed the march's own jitter too, once; that draw
/// comes from the blue-noise tile now, because where a hash puts its error is nobody's decision and
/// a tile's is the whole point of it. What is left wants a hash and not a tile: `fogNoise` asks
/// about a place in the world rather than a pixel on the screen, and there is no screen-space
/// arrangement to arrange.
///
/// Sixteen bits is more than a haze needs, and taking the high ones is what keeps the low bits of a
/// weak avalanche out of the field.
float hashToUnit(ivec3 at)
{
    uvec3 wrapped = uvec3(at);
    uint h = wrapped.x * 1664525u + wrapped.y * 1013904223u + wrapped.z * 2654435761u;
    h ^= h >> 15u;
    h *= 0x2c1b3c6du;
    h ^= h >> 12u;
    h *= 0x297a2d39u;
    h ^= h >> 15u;

    return float(h >> 16u) / 65535.0;
}

/// Which sequence a lamp reservoir draws on. **One per depth, because a path shades twice** — the
/// hit the eye found and the hit its bounce found — and two reservoirs stepping the same sequence
/// would choose correlated lamps at both ends of it. These are seeds for `randomSeed` rather than
/// channels of the tile, which has only `RANDOM_STREAMS` of them and answers a different question.
const uint SEED_LAMPS_EYE = 0x51u;
const uint SEED_LAMPS_BOUNCE = 0x52u;

/// Water shades three surfaces from one hit — what it reflects, what is seen through it, and the
/// raft of foam on top — and each of them opens a reservoir of its own. **Three constants and not
/// one**, because three reservoirs seeded alike step the same sequence and keep the same lamps.
const uint SEED_LAMPS_MIRROR = 0x53u;
const uint SEED_LAMPS_THROUGH = 0x54u;
const uint SEED_LAMPS_FOAM = 0x55u;

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
/// know — which channels are taken — is `STREAM_FOG` and `STREAM_BOUNCE`, and those sit with the
/// count in `scene.h`.
const float STREAM_TURN[RANDOM_STREAMS] = float[](0.6180340, 0.7548777, 0.5698403);

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

#endif
