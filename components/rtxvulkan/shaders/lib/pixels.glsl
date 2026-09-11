#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_PIXELS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_PIXELS_GLSL

// The one test every dispatch in this renderer opens with.
//
// **Nothing is bound here on purpose.** A standalone pass reaches this file without reaching the
// descriptor set, which is what lets `wavecompose.comp` and `histogram.comp` read it beside
// `fogscatter.comp`.

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

/// The same, for the one dispatch whose grid is a volume.
bool outsideOf(uvec3 froxel, uvec3 extent)
{
    return any(greaterThanEqual(froxel, extent));
}

#endif
