// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOOTPRINT_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOOTPRINT_GLSL

// Whether a sampler can still see a field of a given scale.
//
// **A field finer than the sampler looking at it is not detail, it is noise dressed as detail.**
// What `footprint` means is whatever is doing the looking, which here is a ray cone against a
// wavelength. How wide the looking *is* comes off the camera; see `coneAt`.
//
// **Only for a field with no mip chain to climb.** Everything that has one makes this same argument
// exactly and in the sampler — `waveLevel` and `fogFieldAt` each pick the level whose texels are as
// wide as the cone. What is left is `rainSlope`'s rings, which are laid down rather than sampled.

/// How much of something that long a sampler this wide can still tell apart, from none of it to all.
///
/// A ripple narrower than the pixel looking at it is averaged away rather than drawn: a cone a
/// wavelength across covers a crest and a trough whose slopes cancel, and picking one of them
/// instead is what makes distant water a field of crawling white sparks.
float resolved(float wavelength, float footprint)
{
    return 1.0 - smoothstep(0.25 * wavelength, 0.75 * wavelength, footprint);
}

#endif
