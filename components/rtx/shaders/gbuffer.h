#ifndef OPENMW_COMPONENTS_RTX_SHADERS_GBUFFER_H
#define OPENMW_COMPONENTS_RTX_SHADERS_GBUFFER_H

#include "hosttypes.h"
#include "portable.h"

// What each channel of the G-buffer is made of, said once for both sides that have to agree.
//
// **A shader's layout qualifier and the `VkFormat` its image was created with are one fact written
// twice**, and they had drifted: the albedo channel moved to half floats and the two shaders that
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
// So the format is a macro rather than a constant: a layout qualifier is a token GLSL reads before
// it parses anything, and `VK_FORMAT_*` is an enumerator. The preprocessor is the one thing both
// languages share, which is what lets one line define both.

// **The two radiance channels are the one pair with no format here.** How wide they are is a
// run's choice — `Rtx::RadianceWidth` says which run gets which and why — so the host picks
// between `GBUFFER_RADIANCE_SHOWN` and `GBUFFER_RADIANCE_SUMMED` at creation, and every shader that
// reads or writes one declares it with no format at all and lets the load or the store convert.
// `requirements.cpp` asks the device for both halves of that.

#ifdef RTX_HOST

#define GBUFFER_RADIANCE_SHOWN VK_FORMAT_R16G16B16A16_SFLOAT
#define GBUFFER_RADIANCE_SUMMED VK_FORMAT_R32G32B32A32_SFLOAT
#define GBUFFER_ALBEDO VK_FORMAT_R16G16B16A16_SFLOAT
#define GBUFFER_GUIDE VK_FORMAT_R16G16B16A16_SFLOAT
#define GBUFFER_MOTION VK_FORMAT_R16G16_SFLOAT
#define GBUFFER_DEPTH VK_FORMAT_R32G32_SFLOAT
#define GBUFFER_LAYER VK_FORMAT_R16G16B16A16_SFLOAT
#define GBUFFER_PUFF_DEPTH VK_FORMAT_R32G32_SFLOAT
#define GBUFFER_STARS VK_FORMAT_R8G8B8A8_UNORM

#else

#define GBUFFER_ALBEDO rgba16f
#define GBUFFER_GUIDE rgba16f
#define GBUFFER_MOTION rg16f
#define GBUFFER_DEPTH rg32f
#define GBUFFER_LAYER rgba16f
#define GBUFFER_PUFF_DEPTH rg32f
#define GBUFFER_STARS rgba8

#endif

// Which binding of set two each channel is.
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
