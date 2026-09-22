#ifndef OPENMW_COMPONENTS_RTX_SHADERS_GBUFFER_H
#define OPENMW_COMPONENTS_RTX_SHADERS_GBUFFER_H

#include "hosttypes.h"
#include "portable.h"
#include "storageformat.h"

// What each channel of the G-buffer is made of, said once for both sides that have to agree.
//
// **A shader's layout qualifier and the format its image was created with are one fact**, and
// written twice they had drifted: the albedo channel moved to half floats and the two shaders that
// declare it went on saying `rgba32f`. What that costs is not a compile error and not a validation
// *error* — the layers report it as a warning, and the warning says "undefined values to the whole
// image, not just the texel being accessed". A whole channel of the frame, silently, on a
// developer's machine only, because a release build has no layers to say anything at all.
//
// **What the star field is drawn through is three bytes, because every term of it is a
// fraction.** What is left of the field at a pixel is a product of coverages and transmittances,
// each of them from nought to one by construction, so `R8G8B8A8_UNORM` holds the whole range at
// 1/255 steps. Fog thick enough for that step to show is fog no star is visible through. Four
// megabytes at 1080p against the sixteen a half-float image would take for the same three numbers.
//
// **And a motion vector is a half, because a reprojection is now bounded.** It could not be while
// `previousScreen` divided by a distance that approaches nought, which has no bound at all and
// reaches a half float as infinity. `PREVIOUS_SCREEN_REACH` holds it to one screen outside the
// frame either way, so the largest vector a 1920-wide render can carry is 3840 — where a half's
// step is two pixels, on a vector that left the screen twice over. Inside the frame, where a vector
// is read, that step is a sixtieth of a pixel at sixteen and a thousandth at one. NVIDIA's Ray
// Reconstruction guide takes the format.
//
// Eight bytes a pixel across the two motion channels, and 16 MiB of that at 1080p.
//
// **A normal is eleven bits a component, because everything that reads one compares directions.**
// The guide's `xyz` is a unit vector and its `w` a fraction, and the sharpest test made of either is
// the cascade's `pow(dot, 128)`, which cuts a tap at about six degrees of tilt — against the 0.03
// degrees a half float rounds a direction by. Ray Reconstruction asks for this width itself: the
// DLSS-RR integration guide §3.4.3 takes "RGB16_FLOAT or RGB32_FLOAT" with the roughness packed into
// alpha, which is what `DlssPass` already tells it this is.
//
// This is the largest tap in the frame — the cascade reads it twenty-five times a pixel at each of
// five levels — so eight bytes rather than sixteen takes a fifth off that pass's traffic, and
// sixteen megabytes at 1080p rather than thirty-three.
//
// So each format is one line naming a `storageformat.h` layout, which is both the qualifier the
// shader declares and what the host creates the image as.

// **The two radiance channels are the one pair with no format here.** How wide they are is a
// run's choice — `Rtx::RadianceWidth` says which run gets which and why — so the host picks
// between `GBUFFER_RADIANCE_SHOWN` and `GBUFFER_RADIANCE_SUMMED` at creation, and every shader that
// reads or writes one declares it with no format at all and lets the load or the store convert.
// `requirements.cpp` asks the device for both halves of that.

#define GBUFFER_RADIANCE_SHOWN STORAGE_RGBA16F
#define GBUFFER_RADIANCE_SUMMED STORAGE_RGBA32F
#define GBUFFER_ALBEDO STORAGE_RGBA16F
#define GBUFFER_GUIDE STORAGE_RGBA16F
#define GBUFFER_MOTION STORAGE_RG16F
#define GBUFFER_DEPTH STORAGE_RG32F
#define GBUFFER_LAYER STORAGE_RGBA16F
#define GBUFFER_PUFF_DEPTH STORAGE_RG32F
#define GBUFFER_STARS STORAGE_RGBA8

// Which binding of `SET_CHANNELS` each channel is.
//
// **The trace declares them and `GBuffer` writes them, and neither had a name for a single one.**
// The shader spelled a number in each layout qualifier and the C++ built its layout and its writes
// by walking an initializer list, so the two agreed only for as long as nobody reordered the list —
// which is a change that compiles, runs, and hands every pass the wrong image.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// What the trace resolved on its own: direct light, emission, the sky, water and the fog.
    const uint CHANNEL_DIRECT = 0;

    /// The one bounce, demodulated — the only channel a filter may touch.
    const uint CHANNEL_INDIRECT = 1;

    /// What the composite multiplies the bounce back in by, and what an upscaler demodulates each
    /// half of a pixel by.
    const uint CHANNEL_ALBEDO = 2;
    const uint CHANNEL_SPECULAR = 3;

    /// The shading normal and the roughness, which is what a filter and an upscaler compare
    /// surfaces by.
    const uint CHANNEL_GUIDE = 4;

    /// Where things stood on the previous frame's screen, and how far away they are now.
    const uint CHANNEL_MOTION = 5;
    const uint CHANNEL_DEPTH = 6;
    const uint CHANNEL_REFLECTION_MOTION = 7;

    /// How much of the star field a pixel still shows, for the pass that draws it.
    const uint CHANNEL_STARS_SHOWN = 8;

    /// The puffs the trace found in front of the surface, kept apart from it: the sprites and the
    /// cloud shells as one layer, lit where they stand — its colour and what it lets through, then
    /// how far along the ray it stood and what the shells alone let through. What the frame is
    /// composited with comes from here at the traced extent and the sprites' *shape* from a
    /// second march at the shown extent, which `spritecomposite.rgen` puts together — so no puff
    /// goes through a denoiser or an upscaler's overlay.
    const uint CHANNEL_PUFFS = 9;
    const uint CHANNEL_PUFFS_DEPTH = 10;

    /// How many the set declares, which is the last of them and one more.
    const uint CHANNEL_COUNT = 11;

#ifdef RTX_HOST
}
#endif

#endif
