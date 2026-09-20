#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_PIXELS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_PIXELS_GLSL

// The one test every dispatch in this renderer opens with, and where a shown pixel lands on the
// traced grid.
//
// **Nothing is bound here on purpose.** A standalone pass reaches this file without reaching the
// descriptor set, which is what lets `wavecompose.comp` and `histogram.comp` read it beside
// `fogintegrate.comp`.

/// Whether this invocation fell off the edge of what the dispatch covers.
///
/// **One spelling, because twelve passes had four.** A workgroup covers the picture in whole
/// groups, so the last group along each axis runs threads the picture has no pixel for. Each pass
/// said so its own way — an `ivec2` against two casts, a `uvec2` against two fields, an `any` over
/// a `greaterThanEqual` — and a reader had to check that the fourth form meant the first.
bool outsideOf(uvec2 pixel, uvec2 extent)
{
    return any(greaterThanEqual(pixel, extent));
}

/// The same for a tap that may have stepped off the near edge as well as the far one.
bool outsideOf(ivec2 pixel, uvec2 extent)
{
    return any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(uvec2(pixel), extent));
}

/// The traced pixel under a pixel of the shown extent: the one its centre lands on. Nearest and not
/// filtered, because a depth is not a quantity that averages and a tile's list is a list.
///
/// **In integers, because two shaders ask it about one pixel and must land on the same traced
/// one**: the puff composite and the display curve — `puffsCoverNothing` says what they agree
/// about. The centre is at `(p + 1/2) * T / E`, which is `((2p + 1) * T) / (2E)` floored, and a
/// product of two extents fits a word with room to spare. As a float product it sat exactly on a
/// boundary for every third column at `quality`'s two to three, and which side it fell on was the
/// compiler's rounding — different in a launch and in a dispatch. Never past the traced extent:
/// `2p + 1 < 2E`, so the quotient is under `T`.
uvec2 tracedPixelUnder(uvec2 pixel, uvec2 extent, uvec2 tracedExtent)
{
    return ((2u * pixel + 1u) * tracedExtent) / (2u * extent);
}

#endif
