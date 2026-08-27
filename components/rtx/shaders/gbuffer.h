// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_GBUFFER_H
#define OPENMW_COMPONENTS_RTX_SHADERS_GBUFFER_H

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
// **A mask is a byte, because a yes or a no is.** `R8_UNORM` is not among the formats Vulkan
// *requires* a device to support as a storage image, which is why this was a full float first — and
// that was a portability argument in a renderer whose whole posture is that it targets two machines
// and fails loudly on anything either of them cannot do. Measured on the Ada-class NVIDIA part this
// is written against: storage and sampled, both. A device without it fails at image creation
// naming the format, which is the answer this project gives to a missing feature everywhere else.
//
// The two masks between them go from eight megabytes of render-resolution image at 1080p to two.
//
// **And what the star field is drawn through is three bytes, because every term of it is a
// fraction.** What is left of the field at a pixel is a product of coverages and transmittances,
// each of them from nought to one by construction, so `R8G8B8A8_UNORM` holds the whole range at
// 1/255 steps. Fog thick enough for that step to show is fog no star is visible through. Four
// megabytes at 1080p against the sixteen a half-float image would take for the same three numbers.
//
// So the format is a macro rather than a constant: a layout qualifier is a token GLSL reads before
// it parses anything, and `VK_FORMAT_*` is an enumerator. The preprocessor is the one thing both
// languages share, which is what lets one line define both.

#ifdef RTX_HOST

#define GBUFFER_RADIANCE VK_FORMAT_R32G32B32A32_SFLOAT
#define GBUFFER_ALBEDO VK_FORMAT_R16G16B16A16_SFLOAT
#define GBUFFER_GUIDE VK_FORMAT_R32G32B32A32_SFLOAT
#define GBUFFER_MOTION VK_FORMAT_R32G32_SFLOAT
#define GBUFFER_DEPTH VK_FORMAT_R32G32_SFLOAT
#define GBUFFER_MASK VK_FORMAT_R8_UNORM
#define GBUFFER_STARS VK_FORMAT_R8G8B8A8_UNORM

#else

#define GBUFFER_RADIANCE rgba32f
#define GBUFFER_ALBEDO rgba16f
#define GBUFFER_GUIDE rgba32f
#define GBUFFER_MOTION rg32f
#define GBUFFER_DEPTH rg32f
#define GBUFFER_MASK r8
#define GBUFFER_STARS rgba8

#endif

#endif
