// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOOTPRINT_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOOTPRINT_GLSL

// Whether a sampler can still see a field, which a wave, a fog octave and a sprite rim all ask.
//
// **A field finer than the sampler looking at it is not detail, it is noise dressed as detail.**
// What `footprint` means is whatever is doing the looking — a ray cone against a wavelength, a
// march step against an octave. How wide the looking *is* comes off the camera; see `coneAt`.

/// How much of something that long a sampler this wide can still tell apart, from none of it to all.
///
/// A wave narrower than the pixel looking at it is averaged away rather than drawn: a cone a
/// wavelength across covers a crest and a trough whose slopes cancel, and picking one of them
/// instead is what makes distant water a field of crawling white sparks.
///
/// **The fog's octaves ask the same question of the march's step**, which is the same argument with
/// a different sampler: a field finer than the distance between two samples is not detail, it is
/// noise dressed as detail. What `footprint` means is whatever is doing the looking.
float resolved(float wavelength, float footprint)
{
    return 1.0 - smoothstep(0.25 * wavelength, 0.75 * wavelength, footprint);
}

#endif
